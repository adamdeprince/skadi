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

#include "../estruct.h"
#include "../mode.h"
#include "../pico.h"
#include "../edef.h"
#include "../efunc.h"
#include "../keydefs.h"

#include "tty.h"
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
static int	option_glyph_to_letter(int);



void
bail(void)
{
    sleep(30);				/* see if os receives SIGHUP */
    kill(getpid(), SIGHUP);			/* eof or bad error */
}


#define	PASTE_MAX	8192

static UCS paste_q[PASTE_MAX];
static int paste_n = 0;
static int paste_i = 0;
static int paste_fresh = 0;


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


UCS
pico_paste_take(void)
{
    UCS c;

    if(paste_i >= paste_n)
      return(0);

    c = paste_q[paste_i++];
    if(c == '\r'){
	if(paste_i < paste_n && paste_q[paste_i] == '\n')
	  paste_i++;

	return(CTRL | 'M');
    }

    if(c == '\n')
      return(CTRL | 'M');

    if(c >= 0x00 && c <= 0x1F)
      return(CTRL | (c + '@'));

    return(c);
}


int
pico_paste_collect(int (*getcfunc)(int (*)(int), void (*)(void)),
		   int (*recorder)(int), void (*bail_handler)(void),
		   void *data)
{
    UCS ch, status;

    paste_n = paste_i = 0;
    paste_fresh = 0;

    while(paste_n < PASTE_MAX - 1){
	if(!ReadyForKey(1))
	  break;

	status = kbseq(getcfunc, recorder, bail_handler, data, &ch);
	if(status == KEY_PASTE_END)
	  break;

	if(status == KEY_PASTE_START)
	  continue;

	if(status == 0 && ch)
	  paste_q[paste_n++] = ch;
	else
	  break;
    }

    paste_q[paste_n] = 0;
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

    if(pico_paste_queued())
      return(pico_paste_take());

    if(!ReadyForKey(FUDGE-5))
      return(NODATA);

    switch(status = kbseq(simple_ttgetc, NULL, bail, input_cs, &ch)){
      case KEY_PASTE_START:
	pico_paste_collect(simple_ttgetc, NULL, bail, input_cs);
	if(pico_paste_queued())
	  return(pico_paste_take());

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
	{
	    int prev = 0;

	    for(;;){
		if(!ReadyForKey(2))
		  break;

		ch = (*term.t_getchar)(NODATA, NULL, bail);
		if(ch == '\007')
		  break;

		if(prev == '\033' && ch == '\\')
		  break;

		prev = ch;
	    }
	}

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

    {
	/*
	 * Only Option-composed glyphs (∫, ç, ø, ...) become Alt+letter.
	 * ASCII a-z here is ordinary typing — mapping those to META
	 * turned every "o"/"c" into numbered-list / code markup.
	 */
	int letter = option_glyph_to_letter((int) ch);

	if(letter)
	  return(META | letter);
    }

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
#define KB_SEQMAX 64

static int
kb_csi_final(unsigned char c)
{
    return(c >= 0x40 && c <= 0x7e);
}


/*
 * macOS Option+letter glyphs (US layout).  Not Shift, not ASCII.
 */
static int
option_glyph_to_letter(int key)
{
    switch(key){
      case 0x222B:			/* ∫  option+b */
	return('b');
      case 0x02C6:			/* ˆ  option+i */
	return('i');
      case 0x00A8:			/* ¨  option+u */
	return('u');
      case 0x00DF:			/* ß  option+s */
	return('s');
      case 0x00E7:			/* ç  option+c */
	return('c');
      case 0x02D9:			/* ˙  option+h */
	return('h');
      case 0x0153:			/* œ  option+q */
	return('q');
      case 0x03C0:			/* π  option+p */
	return('p');
      case 0x00AE:			/* ®  option+r */
	return('r');
      case 0x00AC:			/* ¬  option+l */
	return('l');
      case 0x00F8:			/* ø  option+o */
	return('o');
      case 0x02DA:			/* ˚  option+k */
	return('k');
      default:
	return(0);
    }
}


/*
 * Map a keycap or macOS Option-composed character to a markup letter.
 * ASCII a-z is for CSI u / modifyOtherKeys, not for unmodded typing.
 */
static int
key_to_meta_letter(int key)
{
    int glyph;

    if(key >= 'A' && key <= 'Z')
      key = key - 'A' + 'a';

    if(key >= 'a' && key <= 'z')
      return(key);

    glyph = option_glyph_to_letter(key);
    return(glyph);
}


static int
kb_parse_int(char **pp, char *end)
{
    char *p = *pp;
    int   v = 0, got = 0;

    while(p < end && *p >= '0' && *p <= '9'){
	v = v * 10 + (*p - '0');
	p++;
	got = 1;
    }

    *pp = p;
    return(got ? v : -1);
}


static UCS
kb_map_mod_key(int key, int mod, UCS *ch)
{
    int bits, alt, ctrl, letter;

    if(mod < 1)
      mod = 1;

    bits = mod - 1;
    alt = bits & 2;
    ctrl = bits & 4;
    letter = key_to_meta_letter(key);

    if(alt && letter)
      return(META | letter);

    if(ctrl){
	if(letter)
	  return(CTRL | (letter - 'a' + 'A'));

	/* CSI u sometimes reports Ctrl+C as key 3 rather than 99 */
	if(key >= 1 && key <= 26)
	  return(CTRL | ('A' + key - 1));

	if(key >= 32 && key < 127)
	  return(CTRL | ((key >= 'a' && key <= 'z')
			 ? (key - 'a' + 'A') : key));
    }

    if(alt || ctrl)
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

    if(key == 127 || key == 8)
      return(0x7F);

    if(key >= 32 && key < 127){
	*ch = (UCS) key;
	return(0);
    }

    if(letter)
      return(META | letter);

    return(BADESC);
}


/*
 * Kitty keyboard protocol: CSI key:shifted:base ; mods:event ; text u
 * xterm modifyOtherKeys:   CSI 27 ; mods ; key ~
 *
 * On macOS, Option+b may show up as key 98 (b), as ∫ (U+222B), or as
 * 98 with ∫ in the associated-text field.  Any of those is Alt+b.
 */
static UCS
kb_decode_csi(unsigned char *seq, int n, UCS *ch)
{
    char *p, *end;
    int   keys[4], nk = 0, mod = 1, ev = 1, text = 0, final;
    int   i, letter, key;

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

    if(final == '~' && keys[0] == 27){
	/* CSI 27 ; mod ; key ~  — key is in the third field (text here) */
	return(kb_map_mod_key(text, mod, ch));
    }

    if(final != 'u')
      return(BADESC);

    letter = 0;
    for(i = 0; i < nk && !letter; i++)
      letter = key_to_meta_letter(keys[i]);

    if(!letter && text)
      letter = key_to_meta_letter(text);

    key = (nk > 0) ? keys[0] : 0;
    if(letter)
      key = letter;

    return(kb_map_mod_key(key, mod, ch));
}


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
