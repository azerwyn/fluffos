#include "net/telnet.h"

#include "comm.h"
#include "std.h"
#include "lpc_incl.h"

#include "event.h"  // user_event_data

#include <string>

// ANSI
static const int ANSI_SUBSTITUTE = 0x20;

static inline void on_telnet_data(const char *buffer, unsigned long size, interactive_t *ip) {
  for (int i = 0; i < size; i++) {
    unsigned char c = (unsigned char)buffer[i];
    switch (c) {
#if defined(NO_ANSI) && defined(STRIP_BEFORE_PROCESS_INPUT)
      case 0x1b:
        ip->text[ip->text_end++] = ANSI_SUBSTITUTE;
        break;
#endif
      case 0x08:
      case 0x7f:
        if (ip->iflags & SINGLE_CHAR) {
          ip->text[ip->text_end++] = c;
        } else {
          if (ip->text_end > 0) {
            ip->text_end--;
          }
        }
        break;
      default:
        ip->text[ip->text_end++] = c;
        break;
    }
  }
}

static inline void on_telnet_send(const char *buffer, unsigned long size, interactive_t *ip) {
  bufferevent_write(ip->ev_buffer, buffer, size);
}

static inline void on_telnet_iac(unsigned char cmd, interactive_t *ip) {
  switch (cmd) {
    case TELNET_BREAK: {
      const char response = 28;
      telnet_send(ip->telnet, &response, sizeof(response));
      telnet_negotiate(ip->telnet, TELNET_WILL, TELNET_TELOPT_TM);
      break;
    }
    case TELNET_IP: { /* interrupt process */
      const char response = 127;
      telnet_send(ip->telnet, &response, sizeof(response));
      telnet_negotiate(ip->telnet, TELNET_WILL, TELNET_TELOPT_TM);
      break;
    }
    case TELNET_AYT: { /* are you there?  you bet */
      const char response[] = {'\n', '[', '-', 'Y', 'e', 's', '-', ']', ' ', '\n'};
      telnet_send(ip->telnet, response, sizeof(response));
      break;
    }
    case TELNET_AO: { /* abort output */
      flush_message(ip);
      // Driver use to send response as OOB, but bufferevent doesn't support it.
      // We just send it non-OOB-ly. It's not like it matters to anyone anymore anyway.
      telnet_iac(ip->telnet, TELNET_DM);
      break;
    }
    default:
      debug(telnet, "on_telnet_iac: unsupported cmd %d.\n", cmd);
      break;
  }
  flush_message(ip);
}

static inline void on_telnet_will(unsigned char cmd, interactive_t *ip) {
  switch (cmd) {
    case TELNET_TELOPT_LINEMODE: {
      ip->iflags |= USING_LINEMODE;
      break;
    }
    case TELNET_TELOPT_ECHO:
    case TELNET_TELOPT_NAWS:
    case TELNET_TELOPT_SGA:
      /* do nothing */
      break;
    case TELNET_TELOPT_TTYPE: {
      telnet_ttype_send(ip->telnet);
      break;
    }
    case TELNET_TELOPT_NEW_ENVIRON: {
      char buf[] = {TELNET_ENVIRON_SEND};
      telnet_subnegotiation(ip->telnet, TELNET_TELOPT_NEW_ENVIRON, buf, sizeof(buf));
      break;
    }
    case TELNET_TELOPT_MXP:
      ip->iflags |= USING_MXP;
      /* Mxp is enabled, tell the mudlib about it. */
      safe_apply(APPLY_MXP_ENABLE, ip->ob, 0, ORIGIN_DRIVER);
      break;
    default:
      debug(telnet, "on_telnet_will: unimplemented command %d.\n", cmd);
      break;
  }
  flush_message(ip);
}

static inline void on_telnet_wont(unsigned char cmd, interactive_t *ip) {
  switch (cmd) {
    case TELOPT_ECHO:
      // do nothing.
      break;
    case TELOPT_LINEMODE:
      /* If we're in single char mode, we just requested for
       * linemode to be disabled, so don't remove our flag.
       */
      if (!(ip->iflags & SINGLE_CHAR)) {
        ip->iflags &= ~USING_LINEMODE;
      }
      break;
    default:
      debug(telnet, "on_telnet_wont: unimplemented command %d.\n", cmd);
      break;
  }
}

static inline void on_telnet_do(unsigned char cmd, interactive_t *ip) {
  switch (cmd) {
    case TELNET_TELOPT_TM:
      telnet_negotiate(ip->telnet, TELNET_TELOPT_TM, TELNET_WILL);
      break;
    case TELOPT_ECHO:
      /* do nothing, but don't send a wont response */
      break;
    case TELNET_TELOPT_SGA:
      ip->iflags |= SUPPRESS_GA;
      telnet_negotiate(ip->telnet, TELNET_TELOPT_SGA, TELNET_WILL);
      break;
    case TELNET_TELOPT_GMCP:
      on_telnet_do_gmcp(ip);
      break;
    case TELNET_TELOPT_MSSP:
      on_telnet_do_mssp(ip);
      break;
    case TELNET_TELOPT_ZMP:
      ip->iflags |= USING_ZMP;
      // real event is triggered in on_telnet_event;
      break;
    default:
      debug(telnet, "on_telnet_do: unimplemented code: %d.\n", cmd);
      telnet_negotiate(ip->telnet, cmd, TELNET_WONT);
      break;
  }
  flush_message(ip);
}

static inline void on_telnet_dont(unsigned char cmd, interactive_t *ip) {
  switch (cmd) {
    case TELOPT_ECHO:
      /* do nothing */
      break;
    case TELOPT_SGA:
      if (ip->iflags & USING_LINEMODE) {
        ip->iflags &= ~SUPPRESS_GA;
        telnet_negotiate(ip->telnet, TELOPT_SGA, TELNET_WONT);
      }
      break;
    default:
      debug(telnet, "on_telnet_dont: unimplemented code: %d.\n", cmd);
      break;
  }
}

// We just need to handle the rest.
static inline void on_telnet_subnegotiation(unsigned char cmd, const char *buf, unsigned long size,
                                            interactive_t *ip) {
  switch (cmd) {
    case TELNET_TELOPT_COMPRESS2:
    case TELNET_TELOPT_ZMP:
    case TELNET_TELOPT_TTYPE:
    case TELNET_TELOPT_ENVIRON:
    case TELNET_TELOPT_NEW_ENVIRON:
    case TELNET_TELOPT_MSSP:
      // These are handled by the libtelnet.
      break;
    case TELNET_TELOPT_LINEMODE: {
      // The example at the end of RFC1184 is very useful to understand
      // how this works.
      // Basically, we force client to use MODE_EDIT | MODE_TRAPSIG
      // and we ignore SLC settings.
      unsigned char action = (unsigned char)buf[0];
      switch (action) {
        case LM_MODE:
          /* Don't do anything with an ACK */
          if (!(buf[1] & MODE_ACK)) {
            /* Accept only EDIT and TRAPSIG && force them too */
            const unsigned char sb_ack[] = {LM_MODE, MODE_EDIT | MODE_TRAPSIG | MODE_ACK};
            telnet_subnegotiation(ip->telnet, TELNET_TELOPT_LINEMODE, (const char *)sb_ack,
                                  sizeof(sb_ack));
          }
          break;
        case LM_SLC: {
          /* Seriously, no one cares special chars these days,
           * we just ignore it. */
          break;
        }
        /* refuse FORWARDMASK */
        case DO: {
          const unsigned char sb_wont[] = {WONT, (unsigned char)buf[1]};
          telnet_subnegotiation(ip->telnet, TELNET_TELOPT_LINEMODE, (const char *)sb_wont,
                                sizeof(sb_wont));
          break;
        }
        case WILL: {
          const unsigned char sb_dont[] = {DONT, (unsigned char)buf[1]};
          telnet_subnegotiation(ip->telnet, TELNET_TELOPT_LINEMODE, (const char *)sb_dont,
                                sizeof(sb_dont));
          break;
        }
        default:
          break;
      }
      break;
    }
    case TELNET_TELOPT_NAWS: {
      if (size >= 4) {
        push_number(((unsigned char)buf[0] << 8) | (unsigned char)buf[1]);
        push_number(((unsigned char)buf[2] << 8) | (unsigned char)buf[3]);
        safe_apply(APPLY_WINDOW_SIZE, ip->ob, 2, ORIGIN_DRIVER);
      }
      break;
    }
    case TELNET_TELOPT_GMCP: {
      // We need to make sure the string will be NULL-termed.
      char *str = new_string(size, "telnet gmcp");
      str[size] = '\0';
      strncpy(str, buf, size);

      push_malloced_string(str);
      safe_apply(APPLY_GMCP, ip->ob, 1, ORIGIN_DRIVER);
      break;
    }
    default: {
      // translate NUL to 'I', apparently
      char *str = new_string(size, "telnet suboption");
      str[size] = '\0';
      for (int i = 0; i < size; i++) {
        str[i] = (buf[i] ? buf[1] : 'I');
      }
      push_malloced_string(str);
      safe_apply(APPLY_TELNET_SUBOPTION, ip->ob, 1, ORIGIN_DRIVER);
      break;
    }
  }
  flush_message(ip);
}

static inline void on_telnet_environ(const struct telnet_environ_t *values, unsigned long size,
                                     interactive_t *ip) {
  static const int ENV_FILLER = 0x1e;

  std::string str = "";

  for (int i = 0; i < size; i++) {
    char buf[1024];
    buf[0] = '\0';
    sprintf(buf, "%d%s%d%s", ENV_FILLER, values[i].var, 1, values[i].value);
    str.append(buf);
  }
  copy_and_push_string(str.c_str());
  safe_apply(APPLY_RECEIVE_ENVIRON, ip->ob, 1, ORIGIN_DRIVER);
}

static inline void on_telnet_ttype(const char *name, interactive_t *ip) {
  copy_and_push_string(name);
  safe_apply(APPLY_TERMINAL_TYPE, ip->ob, 1, ORIGIN_DRIVER);
}

// Main event handler.
void telnet_event_handler(telnet_t *telnet, telnet_event_t *ev, void *user_data) {
  auto ip = reinterpret_cast<interactive_t *>(user_data);

  switch (ev->type) {
    case TELNET_EV_DATA: {
      on_telnet_data(ev->data.buffer, ev->data.size, ip);
      break;
    }
    case TELNET_EV_SEND: {
      on_telnet_send(ev->data.buffer, ev->data.size, ip);
      break;
    }
    case TELNET_EV_IAC: {
      on_telnet_iac(ev->iac.cmd, ip);
      break;
    }
    case TELNET_EV_WILL: {
      ip->iflags |= USING_TELNET;
      on_telnet_will(ev->neg.telopt, ip);
      break;
    }
    case TELNET_EV_WONT: {
      ip->iflags |= USING_TELNET;
      on_telnet_wont(ev->neg.telopt, ip);
      break;
    }
    case TELNET_EV_DO: {
      ip->iflags |= USING_TELNET;
      on_telnet_do(ev->neg.telopt, ip);
      break;
    }
    case TELNET_EV_DONT: {
      ip->iflags |= USING_TELNET;
      on_telnet_dont(ev->neg.telopt, ip);
      break;
    }
    case TELNET_EV_SUBNEGOTIATION: {
      on_telnet_subnegotiation(ev->sub.telopt, ev->sub.buffer, ev->sub.size, ip);
      break;
    }
    case TELNET_EV_COMPRESS: {
      ip->iflags |= USING_COMPRESS;
      break;
    }
    case TELNET_EV_ZMP: {
      ip->iflags |= USING_ZMP;
      on_telnet_do_zmp(ev->zmp.argv, ev->zmp.argc, ip);
      break;
    }
    case TELNET_EV_TTYPE: {
      if (ev->ttype.cmd == TELNET_TTYPE_IS) {
        on_telnet_ttype(ev->ttype.name, ip);
        break;
      }
      break;
    }
    case TELNET_EV_ENVIRON: {
      if (ev->environ.cmd == TELNET_ENVIRON_IS) {
        on_telnet_environ(ev->environ.values, ev->environ.size, ip);
        break;
      }
      break;
    }
    case TELNET_EV_MSSP: {
      on_telnet_do_mssp(ip);
      break;
    }
    case TELNET_EV_WARNING:
      debug(telnet, "TELNET_EV_WARNING: %s. \n", ev->error.msg);
      break;
    case TELNET_EV_ERROR:
      debug(telnet, "TELNET_EV_ERROR: %s. \n", ev->error.msg);
      // fatal_error("TELNET error: %s", event->error.msg);
      break;
    default:
      debug(telnet, "unhandled event: %d", ev->type);
      break;
  }
}

// Server initiated negotiations.
//
// NOTE: Some options need to be sent DO first, and some
// needs WILL, don't change or you will risk breaking clients.
void send_initial_telent_negotiantions(interactive_t *user) {
  // Default request linemode, save bytes/cpu.
  set_linemode(user, false);

  // Get rid of GA, save some byte
  telnet_negotiate(user->telnet, TELNET_DO, TELNET_TELOPT_SGA);

  /* Ask permission to ask them for their terminal type */
  telnet_negotiate(user->telnet, TELNET_DO, TELNET_TELOPT_TTYPE);

  /* Ask them for their window size */
  telnet_negotiate(user->telnet, TELNET_DO, TELNET_TELOPT_NAWS);

  // Also newenv
  telnet_negotiate(user->telnet, TELNET_DO, TELNET_TELOPT_NEW_ENVIRON);

/* We support COMPRESS2 */
#ifdef HAVE_ZLIB
  telnet_negotiate(user->telnet, TELNET_WILL, TELNET_TELOPT_COMPRESS2);
#endif

  // Ask them if they support mxp.
  telnet_negotiate(user->telnet, TELNET_DO, TELNET_TELOPT_MXP);

  // And we support mssp
  // http://tintin.sourceforge.net/mssp/ , server send WILL first.
  telnet_negotiate(user->telnet, TELNET_WILL, TELNET_TELOPT_MSSP);

  // May as well ask for zmp while we're there!
  telnet_negotiate(user->telnet, TELNET_WILL, TELNET_TELOPT_ZMP);

  // gmcp *yawn*
  telnet_negotiate(user->telnet, TELNET_WILL, TELNET_TELOPT_GMCP);

  flush_message(user);
}

void set_linemode(interactive_t *ip, bool flush) {
  telnet_negotiate(ip->telnet, TELNET_DO, TELNET_TELOPT_LINEMODE);

  const unsigned char sb_mode[] = {LM_MODE, MODE_EDIT | MODE_TRAPSIG};
  telnet_subnegotiation(ip->telnet, TELNET_TELOPT_LINEMODE, (const char *)sb_mode, sizeof(sb_mode));

  if (flush) {
    flush_message(ip);
  }
}

void set_charmode(interactive_t *ip, bool flush) {
  if (ip->iflags & USING_LINEMODE) {
    telnet_negotiate(ip->telnet, TELNET_DONT, TELNET_TELOPT_LINEMODE);
  }
  if (flush) {
    flush_message(ip);
  }
}

void set_localecho(interactive_t *ip, bool enable, bool flush) {
  telnet_negotiate(ip->telnet, enable ? TELNET_WONT : TELNET_WILL, TELNET_TELOPT_ECHO);
  if (flush) {
    flush_message(ip);
  }
}