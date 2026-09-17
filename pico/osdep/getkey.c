/*
 * ========================================================================
 * Copyright 2006-2007 University of Washington
 * Copyright 2013-2026 Eduardo Chappa
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * ========================================================================
 */

#include <system.h>
#include <general.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#ifndef _WINDOWS
#include <errno.h>
#include <sys/time.h>
#include <sys/select.h>
#endif

#include "../estruct.h"
#include "../mode.h"
#include "../pico.h"
#include "../edef.h"
#include "../efunc.h"
#include "../keydefs.h"

#include "tty.h"
#include "raw.h"
#include "getkey.h"
#include "read.h"
#include "mouse.h"

#ifdef _WINDOWS
#include "mswin.h"
static int	MapMSKEYtoPK(int c);
#endif /* _WINDOWS */


/* internal declarations */
static int timeo = 0;

/* these next two are declared in pith/conf.h */
int
set_input_timeout(int t)
{
    int oldtimeo = timeo;

    timeo = t;
    return(oldtimeo);
}

int
get_input_timeout(void)
{
    return(timeo);
}


#ifndef _WINDOWS


/* internal prototypes */
void		bail(void);
int		ReadyForKey(int);
static int	key_to_meta_letter(int);
static int	kb_ready_ms(int);
static void	kb_swallow_string(int);



void
bail(void)
{
    sleep(30);				/* see if os receives SIGHUP */
    kill(getpid(), SIGHUP);			/* eof or bad error */
}


/*
 * Bracketed paste.  The whole paste is collected into this queue and
 * handed out one character at a time, so pasted text never spills
 * into the normal key path and control characters in it never run
 * editor commands.
 */
static UCS *paste_q = NULL;
static int  paste_alloc = 0;
static int  paste_n = 0;
static int  paste_i = 0;
static int  paste_fresh = 0;


static int
paste_push(UCS c)
{
    if(paste_n + 1 >= paste_alloc){
	int  na = paste_alloc ? paste_alloc * 2 : 4096;
	UCS *nq = (UCS *) realloc(paste_q, na * sizeof(UCS));

	if(!nq)
	  return(0);

	paste_q = nq;
	paste_alloc = na;
    }

    paste_q[paste_n++] = c;
    paste_q[paste_n] = 0;
    return(1);
}


int
pico_paste_is_fresh(void)
{
    return(paste_fresh && paste_n > 0);
}


void
pico_paste_clear_fresh(void)
{
    paste_fresh = 0;
}


UCS *
pico_paste_ucs4(int *np)
{
    if(np)
      *np = paste_n;

    return(paste_n > 0 ? paste_q : NULL);
}


void
pico_paste_discard(void)
{
    paste_n = paste_i = 0;
    paste_fresh = 0;
}


int
pico_paste_queued(void)
{
    return(paste_i < paste_n);
}


/*
 * Next pasted character for the composer.  Newline and tab come back
 * as the ^M and ^I commands that insert them; any other control
 * character is dropped, so a paste can never run an editor command.
 * Returns 0 when nothing is left.
 */
UCS
pico_paste_take(void)
{
    while(paste_i < paste_n){
	UCS c = paste_q[paste_i++];

	if(c == '\r'){
	    if(paste_i < paste_n && paste_q[paste_i] == '\n')
	      paste_i++;

	    return(CTRL | 'M');
	}

	if(c == '\n')
	  return(CTRL | 'M');

	if(c == '\t')
	  return(CTRL | 'I');

	if(c < 0x20 || c == 0x7F)
	  continue;

	return(c);
    }

    return(0);
}


/*
 * Same, for Alpine's own screens and prompts, which compare against
 * raw C0 bytes rather than pico's CTRL|letter form.
 */
UCS
pico_paste_take_raw(void)
{
    while(paste_i < paste_n){
	UCS c = paste_q[paste_i++];

	if(c == '\r'){
	    if(paste_i < paste_n && paste_q[paste_i] == '\n')
	      paste_i++;

	    return('\r');
	}

	if(c == '\n')
	  return('\r');

	if(c == '\t')
	  return('\t');

	if(c < 0x20 || c == 0x7F)
	  continue;

	return(c);
    }

    return(0);
}


int
pico_paste_collect(int (*getcfunc)(int (*)(int), void (*)(void)),
		   int (*recorder)(int), void (*bail_handler)(void),
		   void *data)
{
    UCS ch, status;

    paste_n = paste_i = 0;
    paste_fresh = 0;

    for(;;){
	/* a paste arrives in one go; a long stall means the end marker is lost */
	if(!ReadyForKey(3))
	  break;

	status = kbseq(getcfunc, recorder, bail_handler, data, &ch);
	if(status == KEY_PASTE_END)
	  break;

	if(status == KEY_PASTE_START)
	  continue;

	if(status == 0){
	    if(ch && !paste_push(ch))
	      break;
	}
	/* anything else (function keys, terminal replies) is not text */
    }

    paste_fresh = (paste_n > 0);
    return(paste_n);
}


#if	TYPEAH
/* 
 * typahead - Check to see if any characters are already in the
 *	      keyboard buffer
 */
int
typahead(void)
{
    int x;	/* holds # of pending chars */

    return((ioctl(0,FIONREAD,&x) < 0) ? 0 : x);
}
#endif /* TYPEAH */



/*
 * ReadyForKey - return true if there's no timeout or we're told input
 *		 is available...
 */
int
ReadyForKey(int timeout)
{
    switch(input_ready(timeout)){
      case READY_TO_READ:
	return(1);
	break;

      case NO_OP_COMMAND:
      case NO_OP_IDLE:
      case READ_INTR:
	return(0);

      case BAIL_OUT:
      case PANIC_NOW:
	emlwwrite(_("Problem reading from keyboard!"), NULL);
	kill(getpid(), SIGHUP);	/* Bomb out (saving our work)! */
	/* no return */
    }

    /* can't happen */
    return(0);
}



/*
 * GetKey - Read in a key.
 * Do the standard keyboard preprocessing. Convert the keys to the internal
 * character set.  Resolves escape sequences and returns no-op if global
 * timeout value exceeded.
 */
UCS
GetKey(void)
{
    UCS ch, status, cc;

    if(pico_paste_queued()){
	UCS pc = pico_paste_take();

	return(pc ? pc : NODATA);
    }

    if(!ReadyForKey(FUDGE-5))
      return(NODATA);

    switch(status = kbseq(simple_ttgetc, NULL, bail, input_cs, &ch)){
      case KEY_PASTE_START:
	pico_paste_collect(simple_ttgetc, NULL, bail, input_cs);
	if(pico_paste_queued()){
	    UCS pc = pico_paste_take();

	    return(pc ? pc : NODATA);
	}

	return(NODATA);

      case KEY_PASTE_END:
	return(NODATA);

      case NODATA:			/* kitty key-release, etc. */
	return(NODATA);

      case 0: 	/* regular character */
	break;

      case KEY_DOUBLE_ESC:
	/*
	 * Special hack to get around comm devices eating control characters.
	 */
	if(!ReadyForKey(5))
	  return(BADESC);		/* user typed ESC ESC, then stopped */
	else
	    switch(status = kbseq(simple_ttgetc, NULL, bail, input_cs, &ch)){
	      case  KEY_UP		:
	      case  KEY_DOWN		:
	      case  KEY_RIGHT		:
	      case  KEY_LEFT		:
	      case  KEY_PGUP		:
	      case  KEY_PGDN		:
	      case  KEY_HOME		:
	      case  KEY_END		:
	      case  KEY_DEL		:
	      case F1  :
	      case F2  :
	      case F3  :
	      case F4  :
	      case F5  :
	      case F6  :
	      case F7  :
	      case F8  :
	      case F9  :
	      case F10 :
	      case F11 :
	      case F12 :
		return(CTRL | status);
		break;

	      case 0: 	/* regular character */
		break;

	      default:				/* punt the whole thing	*/
		(*term.t_beep)();
		return(BADESC);
		break;
	    }

	ch &= 0x7f;
	if(isdigit((unsigned char)ch)){
	    int n = 0, i = ch - '0';

	    if(i < 0 || i > 2)
	      return(BADESC);		/* bogus literal char value */

	    while(n++ < 2){
		if(!ReadyForKey(5)
		   || (!isdigit((unsigned char) (ch =
				   (*term.t_getchar)(NODATA, NULL, bail)))
		       || (n == 1 && i == 2 && ch > '5')
		       || (n == 2 && i == 25 && ch > '5'))){
		    return(BADESC);
		}

		i = (i * 10) + (ch - '0');
	    }

	    ch = i;
	}
	else{
	    if(islower((unsigned char)ch))	/* canonicalize if alpha */
	      ch = toupper((unsigned char)ch);

	    return((isalpha((unsigned char)ch) || ch == '@'
		    || (ch >= '[' && ch <= '_'))
		    ? (CTRL | ch) : ((ch == ' ') ? (CTRL | '@') : ch));
	}

	break;

#ifdef MOUSE
      case KEY_XTERM_MOUSE:
	{
	    /*
	     * Special hack to get mouse events from an xterm.
	     * Get the details, then pass it past the keymenu event
	     * handler, and then to the installed handler if there
	     * is one...
	     */
	    static int    down = 0;
	    int           x, y, button;
	    unsigned long cmd;

	    button = (*term.t_getchar)(NODATA, NULL, bail) & 0x03;

	    x = (*term.t_getchar)(NODATA, NULL, bail) - '!';
	    y = (*term.t_getchar)(NODATA, NULL, bail) - '!';

	    if(button == 0){
		down = 1;
		if(checkmouse(&cmd, 1, x, y))
		  return((UCS) cmd);
	    }
	    else if(down && button == 3){
		down = 0;
		if(checkmouse(&cmd, 0, x, y))
		  return((UCS) cmd);
	    }

	    return(NODATA);
	}

	break;
#endif /* MOUSE */

      case  KEY_UP		:
      case  KEY_DOWN		:
      case  KEY_RIGHT		:
      case  KEY_LEFT		:
      case  KEY_PGUP		:
      case  KEY_PGDN		:
      case  KEY_HOME		:
      case  KEY_END		:
      case  KEY_DEL		:
      case F1  :
      case F2  :
      case F3  :
      case F4  :
      case F5  :
      case F6  :
      case F7  :
      case F8  :
      case F9  :
      case F10 :
      case F11 :
      case F12 :
	return(status);

      case  CTRL_KEY_UP		:
	return(CTRL | KEY_UP);
      case  CTRL_KEY_DOWN	:
	return(CTRL | KEY_DOWN);
      case  CTRL_KEY_RIGHT	:
	return(CTRL | KEY_RIGHT);
      case  CTRL_KEY_LEFT	:
	return(CTRL | KEY_LEFT);

      case KEY_SWALLOW_Z:
	status = BADESC;
      case KEY_SWAL_UP:
      case KEY_SWAL_DOWN:
      case KEY_SWAL_LEFT:
      case KEY_SWAL_RIGHT:
	do
	  if(!ReadyForKey(2)){
	      status = BADESC;
	      break;
	  }
	while(!strchr("~qz", (*term.t_getchar)(NODATA, NULL, bail)));

	return((status == BADESC)
		? status
		: status - (KEY_SWAL_UP - KEY_UP));
	break;

      case KEY_KERMIT:
	do{
	    cc = ch;
	    if(!ReadyForKey(2))
	      return(BADESC);
	    else
	      ch = (*term.t_getchar)(NODATA, NULL, bail) & 0x7f;
	}while(cc != '\033' && ch != '\\');

	ch = NODATA;
	break;

      case KEY_OSC:
      case KEY_APC:
      case KEY_DCS:
	/* a reply from the terminal, not a key */
	kb_swallow_string(status == KEY_OSC);
	return(NODATA);

      case BADESC:
	(*term.t_beep)();
	return(status);

      default:
	/*
	 * Kitty keyboard protocol / modifyOtherKeys deliver Ctrl+letter
	 * as CSI u (CTRL|'C', CTRL|'O', ...) rather than a C0 byte.
	 * Those must run the composer bindings, not beep.
	 */
	if(status & (META | CTRL))
	  return(status);

	(*term.t_beep)();
	return(BADESC);
    }

    if (ch >= 0x00 && ch <= 0x1F)       /* C0 control -> C-     */
      ch = CTRL | (ch+'@');

    return(ch);
}


/* 
 * kbseq - looks at an escape sequence coming from the keyboard and 
 *         compares it to a trie of known keyboard escape sequences, and
 *         returns the function bound to the escape sequence.
 * 
 *     Args: getcfunc     -- Function to get a single character from stdin,
 *                           called with the next two arguments to this
 *                           function as its arguments.
 *           recorder     -- If non-NULL, function used to record keystroke.
 *           bail_handler -- Function used to bail out on read error.
 *           c            -- Pointer to returned character.
 *
 *  Returns: BADESC
 *           The escaped function.
 *           0 if a regular char with char stuffed in location c.
 */
#define KB_SEQMAX	64
#define KB_ESC_MS	50		/* ESC then a key within this is Alt+key */


/*
 * Is there input within ms milliseconds?
 */
static int
kb_ready_ms(int ms)
{
    fd_set rfds;
    struct timeval tv;
    int res;

    for(;;){
	FD_ZERO(&rfds);
	FD_SET(STDIN_FD, &rfds);
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	res = select(STDIN_FD + 1, &rfds, NULL, NULL, &tv);
	if(res < 0 && errno == EINTR)
	  continue;

	return(res > 0);
    }
}


/*
 * Discard a control string (OSC, APC, DCS, PM, SOS) the terminal sent
 * us, up to the string terminator ESC \ (or BEL for OSC).
 */
static void
kb_swallow_string(int bel_ends)
{
    int prev = 0, c;

    for(;;){
	if(!ReadyForKey(2))
	  break;

	c = (*term.t_getchar)(NODATA, NULL, bail);
	if(bel_ends && c == '\007')
	  break;

	if(prev == '\033' && c == '\\')
	  break;

	prev = c;
    }
}


static int
kb_csi_final(unsigned char c)
{
    return(c >= 0x40 && c <= 0x7e);
}

/* CSI-DECODE-BEGIN: keep this region free of pico dependencies (see harness) */


/*
 * The letter an Alt/Ctrl shortcut is about, or 0.  Only ASCII letters;
 * macOS Option-composed glyphs are ordinary text (tell kitty to use
 * the Option key as Alt if you want Alt shortcuts there).
 */
static int
key_to_meta_letter(int key)
{
    if(key >= 'A' && key <= 'Z')
      key = key - 'A' + 'a';

    if(key >= 'a' && key <= 'z')
      return(key);

    return(0);
}


static int
kb_parse_int(char **pp, char *end)
{
    char *p = *pp;
    int   v = 0, got = 0;

    while(p < end && *p >= '0' && *p <= '9'){
	if(v < 10000000)			/* no key code is that large */
	  v = v * 10 + (*p - '0');

	p++;
	got = 1;
    }

    *pp = p;
    return(got ? v : -1);
}


/*
 * Kitty reports keypad and other functional keys with private-use
 * code points.  Map the keypad to what its keys mean; anything else
 * (lock keys, media keys, F13 and up) is not a key we use.
 */
static UCS
kb_keypad_key(int key, UCS *ch)
{
    static const char kp[] = "0123456789./*-+\r=,";

    if(key >= 57399 && key <= 57416){
	*ch = (UCS) kp[key - 57399];
	return(0);
    }

    switch(key){
      case 57417: return(KEY_LEFT);
      case 57418: return(KEY_RIGHT);
      case 57419: return(KEY_UP);
      case 57420: return(KEY_DOWN);
      case 57421: return(KEY_PGUP);
      case 57422: return(KEY_PGDN);
      case 57423: return(KEY_HOME);
      case 57424: return(KEY_END);
      case 57426: return(KEY_DEL);
      default:    return(BADESC);
    }
}


/*
 * key is the unicode key code, shifted the shifted variant the
 * terminal reported (0 if none), mod the CSI modifier value (1 = none).
 * Returns a status like kbseq: 0 with *ch set, META|letter, CTRL|x,
 * or BADESC.
 */
static UCS
kb_map_mod_key(int key, int shifted, int mod, UCS *ch)
{
    int bits, shift, alt, ctrl, letter, k;

    if(mod < 1)
      mod = 1;

    bits = mod - 1;
    shift = bits & 1;
    alt = bits & 2;
    ctrl = bits & 4;

    /* Backspace is Backspace whatever is held down with it */
    if(key == 127 || key == 8){
	*ch = 0x7F;
	return(0);
    }

    letter = key_to_meta_letter(key);

    /*
     * Ctrl+Shift+6 comes as key '6' with shifted key '^'; punctuation
     * shortcuts want the shifted character, letters do not care.
     */
    k = (shift && shifted > 0 && !letter) ? shifted : key;

    if(alt && letter)
      return(META | letter);

    if(ctrl){
	if(letter)
	  return(CTRL | (letter - 'a' + 'A'));

	/* some terminals report Ctrl+C as key 3 rather than 99 */
	if(k >= 1 && k <= 26)
	  return(CTRL | ('A' + k - 1));

	if(k == ' ')
	  return(CTRL | '@');

	if(k >= 0x40 && k <= 0x5F)		/* @ [ \ ] ^ _ */
	  return(CTRL | k);

	if(k >= 0x60 && k <= 0x7E)		/* ` { | } ~ share the row above */
	  return(CTRL | (k - 0x20));

	return(BADESC);
    }

    if(alt)
      return(BADESC);

    if(key == 27){
	*ch = 27;
	return(0);
    }

    if(key == 13){
	*ch = '\r';
	return(0);
    }

    if(key == 9){
	*ch = '\t';
	return(0);
    }

    if(k >= 32){
	*ch = (UCS) k;
	return(0);
    }

    return(BADESC);
}


/*
 * Kitty keyboard protocol: CSI key:shifted:base ; mods:event ; text u
 * xterm modifyOtherKeys:   CSI 27 ; mods ; key ~
 * plus the legacy CSI 1 ; mods X and CSI n ; mods ~ forms.
 */
static UCS
kb_decode_csi(unsigned char *seq, int n, UCS *ch)
{
    char *p, *end;
    int   keys[4], nk = 0, mod = 1, ev = 1, text = 0, final;
    int   key, shifted, base;

    if(n < 3 || seq[0] != '\033' || seq[1] != '[')
      return(BADESC);

    final = seq[n - 1];
    p = (char *) seq + 2;
    end = (char *) seq + n - 1;

    memset(keys, 0, sizeof(keys));
    while(nk < 4 && p < end){
	int v = kb_parse_int(&p, end);

	if(v >= 0)
	  keys[nk++] = v;
	if(p < end && *p == ':'){
	    p++;
	    continue;
	}

	break;
    }

    if(p < end && *p == ';'){
	p++;
	mod = kb_parse_int(&p, end);
	if(mod < 0)
	  mod = 1;
	if(p < end && *p == ':'){
	    p++;
	    ev = kb_parse_int(&p, end);
	    if(ev < 0)
	      ev = 1;
	    while(p < end && *p == ':'){
		p++;
		(void) kb_parse_int(&p, end);
	    }
	}
    }

    if(p < end && *p == ';'){
	p++;
	text = kb_parse_int(&p, end);
	if(text < 0)
	  text = 0;
    }

    if(ev == 3)				/* key-release */
      return(NODATA);

    if(final == '~' && nk == 1 && keys[0] == 27 && text > 0){
	/* xterm modifyOtherKeys: CSI 27 ; mod ; key ~ */
	return(kb_map_mod_key(text, 0, mod, ch));
    }

    if(strchr("ABCDHF", final) && (nk == 0 || keys[0] == 1)){
	/* CSI 1 ; mod X: cursor keys with modifiers */
	int ctrl = (mod - 1) & 4;

	switch(final){
	  case 'A': return(ctrl ? CTRL_KEY_UP : KEY_UP);
	  case 'B': return(ctrl ? CTRL_KEY_DOWN : KEY_DOWN);
	  case 'C': return(ctrl ? CTRL_KEY_RIGHT : KEY_RIGHT);
	  case 'D': return(ctrl ? CTRL_KEY_LEFT : KEY_LEFT);
	  case 'H': return(KEY_HOME);
	  case 'F': return(KEY_END);
	}
    }

    if(final == '~' && nk == 1){
	/* CSI n ; mod ~: editing keys with modifiers */
	switch(keys[0]){
	  case 1: case 7: return(KEY_HOME);
	  case 3: return(KEY_DEL);
	  case 4: case 8: return(KEY_END);
	  case 5: return(KEY_PGUP);
	  case 6: return(KEY_PGDN);
	  default: return(BADESC);
	}
    }

    if(final != 'u')
      return(BADESC);

    key = (nk > 0) ? keys[0] : 0;
    shifted = (nk > 1) ? keys[1] : 0;
    base = (nk > 2) ? keys[2] : 0;
    if(key == 0 && text > 0)
      key = text;

    /*
     * With a non-Latin layout the base-layout key carries the letter
     * a shortcut is named after.
     */
    if(((mod - 1) & 6) && !key_to_meta_letter(key) && key_to_meta_letter(base))
      key = base;

    if(key >= 57344 && key <= 63743)		/* kitty functional keys */
      return(kb_keypad_key(key, ch));

    if(key > 0x10FFFF || shifted > 0x10FFFF)
      return(BADESC);

    return(kb_map_mod_key(key, shifted, mod, ch));
}

/* CSI-DECODE-END */


static UCS
kb_finish_csi(unsigned char *seen, int *nseen,
	      int (*getcfunc)(int (*recorder)(int), void (*bail_handler)(void)),
	      int (*recorder)(int),
	      void (*bail_handler)(void),
	      UCS *ch)
{
    if(*nseen == 2 && seen[0] == '\033' && seen[1] >= 'A' && seen[1] <= 'z'){
	int letter = seen[1];

	/* SS3 / CSI prefixes are not Alt+O / Alt+[ */
	if(letter == 'O' || letter == '[')
	  return(BADESC);

	if(letter >= 'A' && letter <= 'Z')
	  letter = letter - 'A' + 'a';

	if(letter >= 'a' && letter <= 'z')
	  return(META | letter);
    }

    if(*nseen < 2 || seen[0] != '\033' || seen[1] != '[')
      return(BADESC);

    while(*nseen < KB_SEQMAX - 1 && !kb_csi_final(seen[*nseen - 1]))
      seen[(*nseen)++] = (unsigned char) (*getcfunc)(recorder, bail_handler);

    if(!kb_csi_final(seen[*nseen - 1]))
      return(BADESC);

    return(kb_decode_csi(seen, *nseen, ch));
}


UCS
kbseq(int (*getcfunc)(int (*recorder)(int ), void (*bail_handler)(void )),
      int (*recorder)(int),
      void (*bail_handler)(void),
      void *data,
      UCS *ch)
{
    unsigned char c, seen[KB_SEQMAX];
    int      first = 1, nseen = 0;
    KBESC_T *current;

    current = kbesc;
    if(current == NULL)				/* bag it */
      return(BADESC);

    while(1){
	c = (*getcfunc)(recorder, bail_handler);
	if(nseen < KB_SEQMAX - 1)
	  seen[nseen++] = c;

	/*
	 * ESC followed by nothing within KB_ESC_MS is the Escape key,
	 * not the start of an Alt+key or terminal sequence.
	 */
	if(first && c == '\033' && !kb_ready_ms(KB_ESC_MS)){
	    *ch = 27;
	    return(0);
	}

	while(current->value != c){
	    if(current->left == NULL){		/* NO MATCH */
		if(first){
		    unsigned long octets_so_far, remaining_octets;
		    unsigned char *inputp;
		    UCS ucs;
		    unsigned char inputbuf[20];

		    /*
		     * Regular character.
		     * Read enough bytes to make up a character and convert it to UCS-4.
		     */
		    memset(inputbuf, 0, sizeof(inputbuf));
		    inputbuf[0] = c;
		    octets_so_far = 1;
		    for(;;){
			remaining_octets = octets_so_far;
			inputp = inputbuf;
			ucs = mbtow(data, &inputp, &remaining_octets);
			switch(ucs){
			  case CCONV_BADCHAR:
			    /*
			     * Not really a BADESC but that ought to
			     * be sufficient. We can add another type if
			     * we need to.
			     */
			    return(BADESC);
			  
			  case CCONV_NEEDMORE:
			    if(octets_so_far >= sizeof(inputbuf))
			      return(BADESC);

			    c = (*getcfunc)(recorder, bail_handler);
			    inputbuf[octets_so_far++] = c;
			    break;

			  default:
			    /* got a good UCS-4 character */
			    *ch = ucs;
			    return(0);
			}
		    }

		    /* NOTREACHED */
		    return(0);
		}
		else
		  return(kb_finish_csi(seen, &nseen, getcfunc, recorder,
				       bail_handler, ch));
	    }
	    current = current->left;
	}

	if(current->down == NULL)		/* match!!!*/
	  return(current->func);
	else
	  current = current->down;

	first = 0;
    }
}


#define	newnode()	(KBESC_T *)malloc(sizeof(KBESC_T))

/*
 * kpinsert - insert a keystroke escape sequence into the global search
 *	      structure.
 */
void
kpinsert(char *kstr, int kval, int termcap_wins)
{
    register	char	*buf;
    register	KBESC_T *temp;
    register	KBESC_T *trail;

    if(kstr == NULL)
      return;

    /*
     * Don't allow escape sequences that don't start with ESC unless
     * termcap_wins.  This is to protect against mistakes in termcap files.
     */
    if(!termcap_wins && *kstr != '\033')
      return;

    temp = trail = kbesc;
    buf = kstr;

    for(;;){
	if(temp == NULL){
	    temp = newnode();
	    temp->value = *buf;
	    temp->func = 0;
	    temp->left = NULL;
	    temp->down = NULL;
	    if(kbesc == NULL)
	      kbesc = temp;
	    else
	      trail->down = temp;
	}
	else{				/* first entry */
	    while((temp != NULL) && (temp->value != *buf)){
		trail = temp;
		temp = temp->left;
	    }

	    if(temp == NULL){   /* add new val */
		temp = newnode();
		temp->value = *buf;
		temp->func = 0;
		temp->left = NULL;
		temp->down = NULL;
		trail->left = temp;
	    }
	}

	if(*(++buf) == '\0')
	  break;
	else{
	    /*
	     * Ignore attempt to overwrite shorter existing escape sequence.
	     * That means that sequences with higher priority should be
	     * set up first, so if we want termcap sequences to override
	     * hardwired sequences, put the kpinsert calls for the
	     * termcap sequences first.  (That's what you get if you define
	     * TERMCAP_WINS.)
	     */
	    if(temp->func != 0)
	      return;

	    trail = temp;
	    temp = temp->down;
	}
    }
    
    /*
     * Ignore attempt to overwrite longer sequences we are a prefix
     * of (down != NULL) and exact same sequence (func != 0).
     */
    if(temp != NULL && temp->down == NULL && temp->func == 0)
      temp->func = kval;
}



/*
 * kbdestroy() - kills the key pad function key search tree
 *		 and frees all lines associated with it
 *
 *               Should be called with arg kbesc, the top of the tree.
 */
void
kbdestroy(KBESC_T *kb)
{
    if(kb){
	kbdestroy(kb->left);
	kbdestroy(kb->down);
	free((char *)kb);
	kb = NULL;
    }
}

#else /* _WINDOWS */

int
pico_paste_is_fresh(void)
{
    return(0);
}

void
pico_paste_clear_fresh(void)
{
}

UCS *
pico_paste_ucs4(int *np)
{
    if(np)
      *np = 0;

    return(NULL);
}

void
pico_paste_discard(void)
{
}

int
pico_paste_queued(void)
{
    return(0);
}

UCS
pico_paste_take(void)
{
    return(0);
}

UCS
pico_paste_take_raw(void)
{
    return(0);
}

int
pico_paste_collect(int (*getcfunc)(int (*)(int), void (*)(void)),
		   int (*recorder)(int), void (*bail_handler)(void),
		   void *data)
{
    (void) getcfunc;
    (void) recorder;
    (void) bail_handler;
    (void) data;
    return(0);
}



/*
 * Read in a key.
 * Do the standard keyboard preprocessing. Convert the keys to the internal
 * character set.  Resolves escape sequences and returns no-op if global
 * timeout value exceeded.
 */
UCS
GetKey(void)
{
    UCS			ch = 0;
    long		timein;
    

    ch = NODATA;
    timein = time(0L);

    /*
     * Main character processing loop.
     */
    while(!mswin_charavail()) {

#ifdef MOUSE
	/* Check Mouse.  If we get a mouse event, convert to char
	 * event and return that. */
	if (checkmouse (&ch,0,0,0)) {
	    curwp->w_flag |= WFHARD;
	    return (ch);
	}
#endif /* MOUSE */


	/* Check Timeout. */
	if(time(0L) >= timein+(FUDGE-10)) 
	    return(NODATA);
    }

    
    return (mswin_getc_fast());
}

#endif /* _WINDOWS */
