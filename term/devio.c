/*
   Copyright (C) 1995,96,98,99,2000,01 Free Software Foundation, Inc.
   Written by Michael I. Bushnell, p/BSG.

   This file is part of the GNU Hurd.

   The GNU Hurd is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   The GNU Hurd is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA. */

/* Avoid defenition of the baud rates from <ternios.h> at a later time.  */
#include <termios.h>

/* And undefine the baud rates to avoid warnings from
   <device/tty_status.h>.  */
#undef B50
#undef B75
#undef B110
#undef B134
#undef B150
#undef B200
#undef B300
#undef B600
#undef B1200
#undef B1800
#undef B2400
#undef B4800
#undef B9600
#undef B19200
#undef B38400
#undef B57600
#undef B115200
#undef EXTA
#undef EXTB

#include <assert.h>
#include <errno.h>
#include <error.h>
#include <string.h>

#include <device/device.h>
#include <device/device_request.h>
#include <device/tty_status.h>
#include <cthreads.h>

#include <hurd.h>
#include <hurd/ports.h>

#include "term.h"


/* This flag is set if there is an outstanding device_write. */
static int output_pending;

/* This flag is set if there is an outstanding device_read. */
static int input_pending;

/* Tell the status of any pending open */
static enum
{
  NOTPENDING,			/* no open is pending */
  INITIAL,			/* initial open of device pending */
  FAKE,				/* open pending to block on dtr */
} open_pending;

static char pending_output[IO_INBAND_MAX];
static int npending_output;

static struct port_class *phys_reply_class;

/* The Mach device_t representing the terminal. */
static device_t phys_device = MACH_PORT_NULL;

/* The ports we get replies on for device calls. */
static mach_port_t phys_reply_writes = MACH_PORT_NULL;
static mach_port_t phys_reply = MACH_PORT_NULL;

/* The port-info structs. */
static struct port_info *phys_reply_writes_pi;
static struct port_info *phys_reply_pi;

static device_t device_master;

static int output_stopped;

/* XXX Mask that omits high bits we are currently not supposed to pass
   through. */
static int char_size_mask_xxx = 0xff;

/* Forward */
static void devio_desert_dtr ();

static void init_devio (void) __attribute__ ((constructor));
static void
init_devio ()
{
  mach_port_t host_priv;
  error_t err;

  err = get_privileged_ports (&host_priv, &device_master);
  if (err)
    error (1, err, "Getting priviliged ports");
  mach_port_deallocate (mach_task_self (), host_priv);
  phys_reply_class = ports_create_class (0, 0);
}

/* XXX Convert a real speed to a bogus Mach speed.  Return
   -1 if the real speed was bogus, else 0. */
static int
real_speed_to_bogus_speed (int rspeed, int *bspeed)
{
  switch (rspeed)
    {
    case 0:
      *bspeed = B0;
      break;
    case 50:
      *bspeed = B50;
      break;
    case 75:
      *bspeed = B75;
      break;
    case 110:
      *bspeed = B110;
      break;
    case 134:
      *bspeed = B134;
      break;
    case 150:
      *bspeed = B150;
      break;
    case 200:
      *bspeed = B200;
      break;
    case 300:
      *bspeed = B300;
      break;
    case 600:
      *bspeed = B600;
      break;
    case 1200:
      *bspeed = B1200;
      break;
    case 1800:
      *bspeed = B1800;
      break;
    case 2400:
      *bspeed = B2400;
      break;
    case 4800:
      *bspeed = B4800;
      break;
    case 9600:
      *bspeed = B9600;
      break;
    case 19200:
      *bspeed = EXTA;
      break;
    case 38400:
      *bspeed = EXTB;
      break;
#ifdef B57600
    case 57600:
      *bspeed = B57600;
      break;
#endif
#ifdef B115200
    case 115200:
      *bspeed = B115200;
      break;
#endif
    default:
      return -1;
    }
  return 0;
}

/* XXX Convert a bogus speed to a real speed.  */
static int
bogus_speed_to_real_speed (int bspeed)
{
  switch (bspeed)
    {
    case B0:
    default:
      return 0;
    case B50:
      return 50;
    case B75:
      return 75;
    case B110:
      return 110;
    case B134:
      return 134;
    case B150:
      return 150;
    case B200:
      return 200;
    case B300:
      return 300;
    case B600:
      return 600;
    case B1200:
      return 1200;
    case B1800:
      return 1800;
    case B2400:
      return 2400;
    case B4800:
      return 4800;
    case B9600:
      return 9600;
    case EXTA:
      return 19200;
    case EXTB:
      return 38400;
    }
}

/* If there are characters on the output queue and no
   pending output requests, then send them. */
static void
devio_start_output ()
{
  char *cp;
  int size;
  error_t err;

  size = qsize (outputq);

  if (!size || output_pending || (termflags & USER_OUTPUT_SUSP))
    return;

  if (output_stopped)
    {
      device_set_status (phys_device, TTY_START, 0, 0);
      output_stopped = 0;
    }

  /* Copy characters onto PENDING_OUTPUT, not bothering
     those already there. */

  if (size + npending_output > IO_INBAND_MAX)
    size = IO_INBAND_MAX - npending_output;

  cp = pending_output + npending_output;
  npending_output += size;

  while (size--)
    *cp++ = dequeue (outputq);

  /* Submit all the outstanding characters to the device. */
  /* The D_NOWAIT flag does not, in fact, prevent blocks.  Instead,
     it merely causes D_WOULD_BLOCK errors when carrier is down...
     whee.... */
  err = device_write_request_inband (phys_device, phys_reply_writes, D_NOWAIT,
				     0, pending_output, npending_output);

  if (err == MACH_SEND_INVALID_DEST)
    devio_desert_dtr ();
  else if (!err)
    output_pending = 1;
}

error_t
device_write_reply_inband (mach_port_t replypt,
			   error_t return_code,
			   int amount)
{
  if (replypt != phys_reply_writes)
    return EOPNOTSUPP;

  mutex_lock (&global_lock);

  output_pending = 0;

  if (return_code == 0)
    {
      if (amount >= npending_output)
	{
	  npending_output = 0;
	  condition_broadcast (outputq->wait);
	}
      else
	{
	  /* Copy the characters that didn't get output
	     to the front of the array. */
	  npending_output -= amount;
	  memmove (pending_output, pending_output + amount, npending_output);
	}
      devio_start_output ();
    }
  else if (return_code == D_WOULD_BLOCK)
    /* Carrier has dropped. */
    devio_desert_dtr ();
  else
    devio_start_output ();

  mutex_unlock (&global_lock);
  return 0;
}

error_t
device_read_reply_inband (mach_port_t replypt,
			  error_t error_code,
			  char *data,
			  u_int datalen)
{
  int i, flush;
  error_t err;

  if (replypt != phys_reply)
    return EOPNOTSUPP;

  mutex_lock (&global_lock);

  input_pending = 0;

  if (!error_code && (termstate.c_cflag & CREAD))
    for (i = 0; i < datalen; i++)
      {
	int c = data[i];

	/* XXX Mach only supports 8-bit channels; this munges things
	   to account for the reality.  */
	c &= char_size_mask_xxx;

	flush = input_character (c);
	if (flush)
	  break;
      }
  else if (error_code == D_WOULD_BLOCK)
    {
      devio_desert_dtr ();
      mutex_unlock (&global_lock);
      return 0;
    }

  /* D_NOWAIT does not actually prevent blocks; it merely causes
     D_WOULD_BLOCK errors when carrier drops. */
  err = device_read_request_inband (phys_device, phys_reply, D_NOWAIT,
				    0, vm_page_size);

  if (err)
    devio_desert_dtr ();
  else
    input_pending = 1;

  mutex_unlock (&global_lock);
  return 0;
}

static void
devio_set_break ()
{
  device_set_status (phys_device, TTY_SET_BREAK, 0, 0);
}

static void
devio_clear_break ()
{
  device_set_status (phys_device, TTY_CLEAR_BREAK, 0, 0);
}

static void
devio_abandon_physical_output ()
{
  int val = D_WRITE;

  /* If this variable is clear, then carrier is gone, so we
     have nothing to do. */
  if (!phys_reply_writes_pi)
    return;

  mach_port_deallocate (mach_task_self (), phys_reply_writes);
  ports_reallocate_port (phys_reply_writes_pi);
  phys_reply_writes = ports_get_send_right (phys_reply_writes_pi);

  device_set_status (phys_device, TTY_FLUSH, &val, TTY_FLUSH_COUNT);
  npending_output = 0;
  output_pending = 0;
}

static void
devio_suspend_physical_output ()
{
  if (!output_stopped)
    {
      device_set_status (phys_device, TTY_STOP, 0, 0);
      output_stopped = 1;
    }
}

static void
devio_notice_input_flushed ()
{
}

static int
devio_pending_output_size ()
{
  /* Unfortunately, there's no way to get the amount back from Mach
     that has actually been written from this... */
  return npending_output;
}

/* Do this the first time the device is to be opened */
static error_t
initial_open ()
{
  error_t err;

  assert (open_pending != FAKE);

  /* Nothing to do */
  if (open_pending == INITIAL)
    return 0;

  assert (phys_device == MACH_PORT_NULL);
  assert (phys_reply == MACH_PORT_NULL);
  assert (phys_reply_pi == 0);

  err = ports_create_port (phys_reply_class, term_bucket,
			   sizeof (struct port_info), &phys_reply_pi);
  if (err)
    return err;

  phys_reply = ports_get_send_right (phys_reply_pi);

  err = device_open_request (device_master, phys_reply,
			     D_READ|D_WRITE, pterm_name);
  if (err)
    {
      mach_port_deallocate (mach_task_self (), phys_reply);
      phys_reply = MACH_PORT_NULL;
      ports_port_deref (phys_reply_pi);
      phys_reply_pi = 0;
    }
  else
    open_pending = INITIAL;

  return err;
}

static void
devio_desert_dtr ()
{
  int bits;

  /* Turn off DTR. */
  bits = TM_HUP;
  device_set_status (phys_device, TTY_MODEM,
		     (dev_status_t) &bits, TTY_MODEM_COUNT);

  report_carrier_off ();
}

static error_t
devio_assert_dtr ()
{
  error_t err;

  /* The first time is special. */
  if (phys_device == MACH_PORT_NULL)
    return initial_open ();

  /* Schedule a fake open to wait for DTR, unless one is already
     happening. */
  assert (open_pending != INITIAL);
  if (open_pending == FAKE)
    return 0;

  err = device_open_request (device_master, phys_reply,
			     D_READ|D_WRITE, pterm_name);

  if (err)
    return err;

  open_pending = FAKE;
  return 0;
}

kern_return_t
device_open_reply (mach_port_t replyport,
		   int returncode,
		   mach_port_t device)
{
  struct tty_status ttystat;
  int count = TTY_STATUS_COUNT;
  error_t err = 0;

  if (replyport != phys_reply)
    return EOPNOTSUPP;

  mutex_lock (&global_lock);

  assert (open_pending != NOTPENDING);

  if (returncode != 0)
    {
      report_carrier_error (returncode);

      mach_port_deallocate (mach_task_self (), phys_reply);
      phys_reply = MACH_PORT_NULL;
      ports_port_deref (phys_reply_pi);
      phys_reply_pi = 0;

      open_pending = NOTPENDING;
      mutex_unlock (&global_lock);
      return 0;
    }

  if (open_pending == INITIAL)
    {
      /* Special handling for the first open */

      assert (phys_device == MACH_PORT_NULL);
      assert (phys_reply_writes == MACH_PORT_NULL);
      assert (phys_reply_writes_pi == 0);
      phys_device = device;
      err = ports_create_port (phys_reply_class, term_bucket,
			       sizeof (struct port_info),
			       &phys_reply_writes_pi);
      if (err)
	{
	  open_pending = NOTPENDING;
	  mutex_unlock (&global_lock);
	  return err;
	}
      phys_reply_writes = ports_get_send_right (phys_reply_writes_pi);

      /* Schedule our first read */
      err = device_read_request_inband (phys_device, phys_reply, D_NOWAIT,
					0, vm_page_size);

      input_pending = 1;
    }
  else
    {
      /* This was a fake open, only for the sake of assert DTR. */
      device_close (device);
      mach_port_deallocate (mach_task_self (), device);
    }

  device_get_status (phys_device, TTY_STATUS,
		     (dev_status_t)&ttystat, &count);
  ttystat.tt_breakc = 0;
  ttystat.tt_flags = TF_ANYP | TF_LITOUT | TF_NOHANG | TF_HUPCLS;
  device_set_status (phys_device, TTY_STATUS,
		     (dev_status_t)&ttystat, TTY_STATUS_COUNT);

  report_carrier_on ();
  if (err)
    devio_desert_dtr ();

  open_pending = NOTPENDING;
  mutex_unlock (&global_lock);

  return 0;
}

/* Adjust physical state on the basis of the terminal state.
   Where it isn't possible, mutate terminal state to match
   reality. */
static void
devio_set_bits ()
{
  if (!(termstate.c_cflag & CIGNORE) && phys_device != MACH_PORT_NULL)
    {
      struct tty_status ttystat;
      int cnt = TTY_STATUS_COUNT;

      /* Find the current state. */
      device_get_status (phys_device, TTY_STATUS, (dev_status_t) &ttystat, &cnt);
      if (termstate.__ispeed)
	real_speed_to_bogus_speed (termstate.__ispeed, &ttystat.tt_ispeed);
      if (termstate.__ospeed)
	real_speed_to_bogus_speed (termstate.__ospeed, &ttystat.tt_ospeed);

      /* Try and set it. */
      device_set_status (phys_device, TTY_STATUS,
			 (dev_status_t) &ttystat, TTY_STATUS_COUNT);

      /* And now make termstate match reality. */
      cnt = TTY_STATUS_COUNT;
      device_get_status (phys_device, TTY_STATUS, (dev_status_t) &ttystat, &cnt);
      termstate.__ispeed = bogus_speed_to_real_speed (ttystat.tt_ispeed);
      termstate.__ospeed = bogus_speed_to_real_speed (ttystat.tt_ospeed);

      /* Mach forces us to use the normal stop bit convention:
	 two bits at 110 bps; 1 bit otherwise. */
      if (termstate.__ispeed == 110)
	termstate.c_cflag |= CSTOPB;
      else
	termstate.c_cflag &= ~CSTOPB;

      /* Figure out how to munge input, since we are unable to actually
	 affect what the hardware does. */
      switch (termstate.c_cflag & CSIZE)
	{
	case CS5:
	  char_size_mask_xxx = 0x1f;
	  break;

	case CS6:
	  char_size_mask_xxx = 0x3f;
	  break;

	case CS7:
	  char_size_mask_xxx = 0x7f;
	  break;

	case CS8:
	default:
	  char_size_mask_xxx = 0xff;
	  break;
	}
      if (termstate.c_cflag & PARENB)
	char_size_mask_xxx |= 0x80;
    }
}

static void
devio_mdmctl (int how, int bits)
{
  int oldbits, newbits;
  int cnt;
  if ((how == MDMCTL_BIS) || (how == MDMCTL_BIC))
    {
      cnt = TTY_MODEM_COUNT;
      device_get_status (phys_device, TTY_MODEM,
			 (dev_status_t) &oldbits, &cnt);
      if (cnt < TTY_MODEM_COUNT)
	oldbits = 0;		/* what else can we do? */
    }

  if (how == MDMCTL_BIS)
    newbits = (oldbits | bits);
  else if (how == MDMCTL_BIC)
    newbits = (oldbits &= ~bits);
  else
    newbits = bits;

  device_set_status (phys_device, TTY_MODEM,
		     (dev_status_t) &newbits, TTY_MODEM_COUNT);
}

static int
devio_mdmstate ()
{
  int bits, cnt;

  cnt = TTY_MODEM_COUNT;
  device_get_status (phys_device, TTY_MODEM, (dev_status_t) &bits, &cnt);
  if (cnt != TTY_MODEM_COUNT)
    return 0;
  else
    return bits;
}

/* Unused stubs */
kern_return_t
device_read_reply (mach_port_t port,
		   kern_return_t retcode,
		   io_buf_ptr_t data,
		   mach_msg_type_number_t cnt)
{
  return EOPNOTSUPP;
}

kern_return_t
device_write_reply (mach_port_t replyport,
		    kern_return_t retcode,
		    int written)
{
  return EOPNOTSUPP;
}

error_t
ports_do_mach_notify_send_once (mach_port_t notify)
{
  error_t err;

  mutex_lock (&global_lock);

  if (notify == phys_reply_writes)
    {
      err = 0;
      devio_start_output ();
    }
  else if (notify == phys_reply)
    {
      if (input_pending)
	{
	  /* xxx */
	  char msg[] = "Term input check happened\r\n";
	  int foo;
	  device_write_inband (phys_device, 0, 0, msg, sizeof msg, &foo);
	  /* end xxx */

	  input_pending = 0;

	  err = device_read_request_inband (phys_device, phys_reply,
					    D_NOWAIT, 0, vm_page_size);
	  if (err)
	    devio_desert_dtr ();
	  else
	    input_pending = 1;
	}
      else if (open_pending != NOTPENDING)
	{
	  open_pending = NOTPENDING;

	  report_carrier_on ();
	  report_carrier_off ();

	  mach_port_deallocate (mach_task_self (), phys_reply);
	  phys_reply = MACH_PORT_NULL;
	  ports_port_deref (phys_reply_pi);
	  phys_reply_pi = 0;
	}
      err = 0;
    }
  else
    err = EOPNOTSUPP;

  mutex_unlock (&global_lock);
  return err;
}


struct bottomhalf devio_bottom =
{
  devio_start_output,
  devio_set_break,
  devio_clear_break,
  devio_abandon_physical_output,
  devio_suspend_physical_output,
  devio_pending_output_size,
  devio_notice_input_flushed,
  devio_assert_dtr,
  devio_desert_dtr,
  devio_set_bits,
  devio_mdmctl,
  devio_mdmstate,
};
