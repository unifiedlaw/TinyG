/*
 * controller.c - tinyg controller and top level parser
 * Part of TinyG project
 *
 * Copyright (c) 2010 Alden S. Hart, Jr.
 *
 * TinyG is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free 
 * Software Foundation, either version 3 of the License, or (at your 
 * option) any later version.
 *
 * TinyG is distributed in the hope that it will be useful, but WITHOUT 
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or 
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License 
 * for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with TinyG  If not, see <http://www.gnu.org/licenses/>.
 *
 * ---- Controller Operation ----
 *
 *	The controller implements a simple process control scheme to manage 
 *	blocking of multiple "threads" in the application. Technically, the 
 *	controller implements a simple event-drive state machine using inverted
 *	control to manage a set of cooperative run-to-completion kernel tasks.
 * 	(ref: http://www.state-machine.com/products)
 *
 *	More simply, it works as a aborting "superloop", where the highest 
 *	priority tasks are run first and progressively lower priority tasks 
 *	are run only if the higher priority tasks are not blocked. No task 
 *	should ever actually block, but instead return "busy" when it would
 *	ordinarily block, and provide a re-entry point to resume the task
 *	once the blocking condition has been removed.
 *
 *	For this to work tasks must be written to run-to-completion (non 
 *	blocking), and must offer re-entry points (continuations) to resume
 *	operations that would have blocked (see arc generator for an example). 
 *	A task returns TG_EAGAIN to indicate a blocking point. If TG_EAGAIN 
 *	is received The controller quits the loop and starts over. Any other 
 *	return code allows the controller to proceed down the task list.
 *	(See end notes in this file for how to write a continuation).
 *
 *	Interrupts run at the highest priority level; kernel tasks are 
 *	organized into priority groups below the interrupt levels. The 
 *	priority of operations is:
 *
 *	- High priority ISRs
 *		- issue steps to motors / count dwell timings
 *		- dequeue and load next stepper move
 *
 *	- Medium priority ISRs
 *		- receive serial input (RX)
 *		- execute signals received by serial input
 *		- detect and flag limit switch closures
 *
 *	- Low priority ISRs
 *		- send serial output (TX)
 *
 *	- Main loop tasks
 *		These are divided up into layers depending on priority and blocking
 *		hierarchy. See tg_controller() for details.
 *
 *	Notes:
 *	  - Gcode and other command line flow control is managed cooperatively
 *		with the application sending Gcode or other commands. The '*' 
 *		char in the prompt indicates that the controller is ready for the 
 *		next line. The sending app is supposed to honor this and not stuff
 *		lines down the pipe (which will choke the controller).
 *
 *	Futures: Using a super loop instead of an event system is a design 
 *	tradoff - or more to the point - a hack. If the flow of control gets 
 *	much more complicated it will make sense to replace this section 
 *	with an event driven dispatcher.
 *
 * ---- Mode Auto-Detection behaviors ----
 *
 *	The first letter of an IDLE mode line performs the following actions
 *
 *		G,M,N,F,%,(	enter GCODE_MODE (as will lower-case of the same)
 *		C,?			enter CONFIG_MODE
 *		D,A			enter DIRECT_DRIVE_MODE
 *		F			enter FILE_MODE (returns automatically after file ends)
 *		H			help screen (returns to IDLE mode)
 *		T			execute test (whatever you link into it)
 *		I			<reserved>
 *		V			<reserved>
 *
 *	Once in the selected mode these characters are not active as mode 
 *	selects. Most modes use Q (Quit) to exit and return to idle mode.
 */

#include <stdio.h>
#include <ctype.h>
#include <avr/pgmspace.h>

#include "xio.h"
#include "tinyg.h"
#include "controller.h"
#include "gcode.h"				// calls out to gcode parser, etc.
#include "config.h"				// calls out to config parser, etc.
#include "move_queue.h"
#include "canonical_machine.h"	// uses homing cycle
#include "motion_control.h"
#include "direct_drive.h"
#include "stepper.h"			// needed for stepper kill and terminate
#include "limit_switches.h"
#include "xmega_eeprom.h"

/*
 * Canned gcode files for testing
 */

//#include "gcode_tests.h"		// system tests and other assorted test code
//#include "gcode_zoetrope.h"	// zoetrope moves. makes really cool sounds
//#include "gcode_mudflap.h"
//#include "gcode_contraptor_circle.h"
#include "gcode_hokanson.h"

/*
 * Local Scope Functions and Data
 */

struct tgController tg;			// controller state structure

static int _tg_parser(char * buf);
static int _tg_read_next_line(void);
static void _tg_prompt(void);
static void _tg_set_mode(uint8_t mode);
static void _tg_set_source(uint8_t d);
static int _tg_reset(void);
static int _tg_test(void);
//static int _tg_mudflap_file(void);

/*
 * tg_init() - controller init
 * tg_alive() - announce that TinyG is alive
 *
 *	The controller init is split in two: the actual init, and tg_alive()
 *	which should be issued once the rest of he application is initialized.
 */

void tg_init() 
{
	// set input source
	tg.default_src = DEFAULT_SOURCE;// set in tinyg.h
	_tg_set_source(tg.default_src);	// set initial active source
	_tg_set_mode(TG_IDLE_MODE);		// set initial operating mode
	tg.state = TG_READY_UNPROMPTED;
}

void tg_alive() 
{										 // see tinyg.h for string
	printf_P(PSTR("**** TinyG %S ****\n"), (PSTR(TINYG_VERSION)));
	_tg_prompt();
}

/* 
 * tg_controller() - top-level controller
 *
 * Tasks are ordered by increasing dependency (blocking hierarchy) - tasks 
 * that are dependent on the completion of lower-level tasks should be
 * placed later in the list than the task(s) they are dependent upon.
 * Any task in the list must be written as a continuation. See end notes
 * in this file for how to code continuations.
 */

#define	DISPATCH(func) switch (func) { \
	case (TG_EAGAIN): { return; } \
	case (TG_OK): { if (cfg.cycle_active) { return; } \
					else { tg.state = TG_READY_UNPROMPTED; _tg_prompt(); return; \
				  } } }
	// Any other condition drops through and runs the next routine in the list
	// A routine returning TG_EAGAIN pre-empts the rest of the list
	// A routine returning TG_OK will cause a prompt to occur
	// A routine that had no action (i.e. is OFF or idle) should return TG_NOOP 

void tg_controller()
{
	// level 0 routines - main loop routines invoked from interrupts
	DISPATCH(ls_handler());				// limit switch handler
//	DISPATCH(tg_process_end());			// complete processing ENDs

	// level 1 routines - stepper and motion primitives
	st_execute_move();					// always do this (no dispatch)
	DISPATCH(mc_line_continue());
	DISPATCH(mc_dwell_continue());
	DISPATCH(mc_queued_start_stop_continue());
	DISPATCH(mc_arc_continue());

	// level 2 routines - canonical machine cycles
	DISPATCH(cm_return_to_home_continue());

	// level 3 routines - parsers and line readers
	DISPATCH(_tg_read_next_line());
	
	// level N - always end with the prompt
	_tg_prompt();
}

/* 
 * tg_application_startup() - application start and restart
 */

uint8_t tg_application_startup(void)
{
	tg.status = TG_OK;					// pre-emptively set

	// inititate homing cycle
	if (cfg.homing_mode == TRUE) { 
		tg.status = cm_return_to_home();
	}

	// Pre-load the USB RX (input) buffer with some test strings
	// Be mindful of the char limit on the RX_BUFFER_SIZE (circular buffer)

//	xio_queue_RX_string_usb("?\n");		// enter config mode and dump config
//	xio_queue_RX_string_usb("Q\n");		// go to idle mode
//	xio_queue_RX_string_usb("R\n");		// run a homing cycle
//	xio_queue_RX_string_usb("Q\n");		// go to idle mode
//	xio_queue_RX_string_usb("T\n");		// run test file

//	xio_queue_RX_char_usb(ETX);			// send control-c (kill)
//	xio_queue_RX_char_usb(XOFF);		// send control-s (pause)

//	xio_queue_RX_string_usb("(MSGtest message in comment)\n");
//	xio_queue_RX_string_usb("g1 f450 x10 y13\n");
//	xio_queue_RX_string_usb("g0x0y0z0\n");
//	xio_queue_RX_string_usb("g0x1000\n");
//	xio_queue_RX_string_usb("g0x10000\n");
//	xio_queue_RX_string_usb("g0x10\ng4p1\ng0x0\n");
//	xio_queue_RX_string_usb("g0 x-10 (MSGtest)\n");
//	xio_queue_RX_string_usb("g0 x10 y11 z12\n");
//	xio_queue_RX_string_usb("g0 x0 y0 z0\n");
//	xio_queue_RX_string_usb("g2 x10 y10 z0 i5 j5 f200\n");
//	xio_queue_RX_string_usb("g2 f300 x10 y10 i8 j8\n");
//	xio_queue_RX_string_usb("g92 x0 y0 z0\n");
//	xio_queue_RX_string_usb("g0x10y10z0\n");
//	xio_queue_RX_string_usb("g91g0x5y5\n");
//	xio_queue_RX_string_usb("g1 f200 x10\n");
//	xio_queue_RX_string_usb("y10\n");
//	xio_queue_RX_string_usb("g0 x100 y100 z100 a100\n");
//	xio_queue_RX_string_usb("?\n");

	return (tg.status);
}


/* 
 * _tg_read_next_line() - Perform a non-blocking line read from active input device
 */

int _tg_read_next_line()
{
	// read input line or return if not a completed line
	if ((tg.status = xio_gets(tg.src, tg.buf, sizeof(tg.buf))) == TG_OK) {
		tg.status = _tg_parser(tg.buf);			// dispatch to parser
	}

	// Note: This switch statement could be reduced as most paths lead to
	//		 TG_READY_UNPROMPTED, but it's written for clarity instead.
	switch (tg.status) {

		case TG_EAGAIN: case TG_NOOP: break;	// no change of state

		case TG_OK: {							// finished a line OK
			tg.state = TG_READY_UNPROMPTED; 	// ready for next input line
			break;
		}
		case TG_QUIT: {							// Quit returned from parser
			_tg_set_mode(TG_IDLE_MODE);
			tg.state = TG_READY_UNPROMPTED;
			break;
		}
		case TG_EOF: {							// EOF from file devs only
			printf_P(PSTR("End of command file\n"));
			tg_reset_source();					// reset to default src
//			xio_queue_RX_string_usb("Q\nT\n");	// +++++ run test file again (forever)
			tg.state = TG_READY_UNPROMPTED;
			break;
		}
		default: {
			tg.state = TG_READY_UNPROMPTED;		// traps various errors
		}
	}
	return (tg.status);
}

/* 
 * _tg_parser() - process top-level serial input 
 *
 *	tg_parser is the top-level of the input parser tree; dispatches other 
 *	parsers. Calls lower level parser based on mode
 *
 *	Keeps the system MODE, one of:
 *		- control mode (no lines are interpreted, just control characters)
 *		- config mode
 *		- direct drive mode
 *		- gcode mode
 *
 *	In control mode it auto-detects mode by first character of input buffer
 *	Quits from a parser are handled by the controller (not individual parsers)
 *	Preserves and passes through return codes (status codes) from lower levels
 */

int _tg_parser(char * buf)
{
	// auto-detect mode if not already set 
	if (tg.mode == TG_IDLE_MODE) {
		switch (toupper(buf[0])) {
			case 'G': case 'M': case 'N': case 'F': case '(': case '%': case '\\':
				_tg_set_mode(TG_GCODE_MODE); break;
			case 'C': case '?': _tg_set_mode(TG_CONFIG_MODE); break;
			case 'D': _tg_set_mode(TG_DIRECT_DRIVE_MODE); break;
			case 'R': return (_tg_reset());
			case 'T': return (_tg_test());		// run whatever test you want
//			case 'H': return (_tg_help_file());
//			case 'Q': return (_tg_mudflap_file());
//			case 'I': return (_tg_reserved());	// reserved
//			case 'V': return (_tg_reserved());	// reserved
			default:  _tg_set_mode(TG_IDLE_MODE); break;
		}
	}
	// dispatch based on mode
	tg.status = TG_OK;
	switch (tg.mode) {
		case TG_CONFIG_MODE: tg.status = cfg_parse(buf, TRUE, TRUE); break;
		case TG_GCODE_MODE: tg.status = gc_gcode_parser(buf); break;
		case TG_DIRECT_DRIVE_MODE: tg.status = dd_parser(buf); break;
	}
	return (tg.status);
}

/*
 * _tg_set_mode() - Set current operating mode
 */

void _tg_set_mode(uint8_t mode)
{
	tg.mode = mode;
}

/*
 * tg_reset_source()  Reset source to default input device
 * _tg_set_source()  Set current input source
 *
 * Note: Once multiple serial devices are supported this function should 
 *	be expanded to also set the stdout/stderr console device so the prompt
 *	and other messages are sent to the active device.
 */

void tg_reset_source()
{
	_tg_set_source(tg.default_src);
}

void _tg_set_source(uint8_t d)
{
	tg.src = d;							// d = XIO device #. See xio.h
	if (tg.src == XIO_DEV_PGM) {
		tg.flags &= ~TG_FLAG_PROMPTS_bm;
	} else {
		tg.flags |= TG_FLAG_PROMPTS_bm;
	}
}

/* 
 * _tg_prompt() - conditionally display command line prompt
 *
 * We only want a prompt if the following conditions apply:
 *	- system is ready for the next line of input
 *	- no prompt has been issued (issue only one)
 *
 * Further, we only want an asterisk in the prompt if it's not a file device.
 *
 * ---- Mode Strings - for ASCII output ----
 *	This is an example of how to put a string table into program memory
 *	The order of strings in the table must match order of prModeTypes enum
 *	Access is by: (PGM_P)pgm_read_word(&(tgModeStrings[i]));
 *	  where i is the tgModeTypes enum, e.g. modeGCode
 *
 *	ref: http://www.cs.mun.ca/~paul/cs4723/material/atmel/avr-libc-user-manual-1.6.5/pgmspace.html
 *	ref: http://johnsantic.com/comp/state.html, "Writing Efficient State Machines in C"
 */

char tgModeStringIdle[] PROGMEM = "IDLE MODE"; // put strings in program memory
char tgModeStringConfig[] PROGMEM = "CONFIG MODE";
char tgModeStringGCode[] PROGMEM = "G-CODE MODE";
char tgModeStringDirect[] PROGMEM = "DIRECT DRIVE";

PGM_P tgModeStrings[] PROGMEM = {	// put string pointer array in program memory
	tgModeStringIdle,
	tgModeStringConfig,
	tgModeStringGCode,
	tgModeStringDirect
};

void _tg_prompt()
{
	if (tg.state == TG_READY_UNPROMPTED) {
		if (tg.flags && TG_FLAG_PROMPTS_bm) {
			printf_P(PSTR("TinyG [%S]*> "),(PGM_P)pgm_read_word(&tgModeStrings[tg.mode]));
		}
		tg.state = TG_READY_PROMPTED;
	}
}

/*
 * tg_print_status()
 *
 *	Send status message to stderr. "Case out" common messages 
 */

// put strings in program memory
char tgs00[] PROGMEM = "OK";
char tgs01[] PROGMEM = "{01} ERROR";
char tgs02[] PROGMEM = "{02} EAGAIN";
char tgs03[] PROGMEM = "{03} NOOP";
char tgs04[] PROGMEM = "{04} End of line";
char tgs05[] PROGMEM = "{05} End of file";
char tgs06[] PROGMEM = "{06} File not open";
char tgs07[] PROGMEM = "{07} Max file size exceeded";
char tgs08[] PROGMEM = "{08} No such device";
char tgs09[] PROGMEM = "{09} Buffer empty";
char tgs10[] PROGMEM = "{10} Buffer full - fatal";
char tgs11[] PROGMEM = "{11} Buffer full - non-fatal";
char tgs12[] PROGMEM = "{12} QUIT";
char tgs13[] PROGMEM = "{13} Unrecognized command";
char tgs14[] PROGMEM = "{14} Expected command letter";
char tgs15[] PROGMEM = "{15} Unsupported statement";
char tgs16[] PROGMEM = "{16} Parameter over range";
char tgs17[] PROGMEM = "{17} Bad number format";
char tgs18[] PROGMEM = "{18} Floating point error";
char tgs19[] PROGMEM = "{19} Motion control error";
char tgs20[] PROGMEM = "{20} Arc specification error";
char tgs21[] PROGMEM = "{21} Zero length line";
char tgs22[] PROGMEM = "{22} Maximum feed rate exceeded";
char tgs23[] PROGMEM = "{23} Maximum seek rate exceeded";
char tgs24[] PROGMEM = "{24} Maximum table travel exceeded";
char tgs25[] PROGMEM = "{25} Maximum spindle speed exceeded";

// put string pointer array in program memory. MUST BE SAME COUNT AS ABOVE
PGM_P tgStatus[] PROGMEM = {	
	tgs00, tgs01, tgs02, tgs03, tgs04, tgs05, tgs06, tgs07, tgs08, tgs09,
	tgs10, tgs11, tgs12, tgs13, tgs14, tgs15, tgs16, tgs17, tgs18, tgs19,
	tgs20, tgs21, tgs22, tgs23, tgs24, tgs25
};

void tg_print_status(const uint8_t status_code, const char *textbuf)
{
	switch (status_code) {		// don't send messages for these status codes
		case TG_OK: return;
		case TG_EAGAIN: return;
		case TG_NOOP: return;
		case TG_QUIT: return;
		case TG_ZERO_LENGTH_MOVE: return;
	}
	printf_P(PSTR("%S: %s\n"),(PGM_P)pgm_read_word(&tgStatus[status_code]), textbuf);
//	printf_P(PSTR("%S\n"),(PGM_P)pgm_read_word(&tgStatus[status_code]));
}

/*
 * _tg_reset() - run power-up resets, including homing (table zero)
 */

int _tg_reset(void)
{
//	tg_application_init();				// application hard init
	tg.status = tg_application_startup();			// application startup sequence

	if (cfg.homing_mode) {
		tg.status = cm_return_to_home(); // do a homing cycle
	} else {
		tg.status = (TG_OK);
	}
	return(tg.status);
}

/*
 * _tg_test() - run a test file from program memory
 */

int _tg_test(void)
{
//	xio_open_pgm(PGMFILE(&system_test)); // collected system tests
//	xio_open_pgm(PGMFILE(&straight_feed_test));
//	xio_open_pgm(PGMFILE(&arc_feed_test));
//	xio_open_pgm(PGMFILE(&contraptor_circle)); 	// contraptor circle test
	xio_open_pgm(PGMFILE(&hokanson_02)); 	// contraptor circle test
	_tg_set_source(XIO_DEV_PGM);
	_tg_set_mode(TG_GCODE_MODE);
	return (TG_OK);
}

/*
 * _tg_run_file() - selects and starts playback from a memory file
 */
/*
int _tg_run_file()
{
	// Open a program memory file:
//	xio_open_pgm(PGMFILE(&g0_test1));		// simple linear motion test
//	xio_open_pgm(PGMFILE(&g0_test2));		// another simple linear motion test
//	xio_open_pgm(PGMFILE(&g0_test3));		// very short moves for single stepping
//	xio_open_pgm(PGMFILE(&radius_arc_test1));
//	xio_open_pgm(PGMFILE(&radius_arc_test2));
//	xio_open_pgm(PGMFILE(&square_test1));
//	xio_open_pgm(PGMFILE(&square_test2));
//	xio_open_pgm(PGMFILE(&square_test10));
//	xio_open_pgm(PGMFILE(&circle_test10));
//	xio_open_pgm(PGMFILE(&square_circle_test10));
//	xio_open_pgm(PGMFILE(&square_circle_test100));
//	xio_open_pgm(PGMFILE(&spiral_test50a));
//	xio_open_pgm(PGMFILE(&spiral_test5));
//	xio_open_pgm(PGMFILE(&dwell_test2));

//	xio_open_pgm(PGMFILE(&zoetrope));			// crazy noisy zoetrope file
//	xio_open_pgm(PGMFILE(&mudflap)); 			// mudflap girl
//	xio_open_pgm(PGMFILE(&roadrunner));

//	xio_open_pgm(PGMFILE(&parser_test1));		// gcode parser tests

	// set source and mode
	_tg_set_source(XIO_DEV_PGM);
	_tg_set_mode(TG_GCODE_MODE);
	return (TG_OK);
}

int _tg_mudflap_file()
{
	xio_open_pgm(PGMFILE(&mudflap)); 			// mudflap girl
	_tg_set_source(XIO_DEV_PGM);
	_tg_set_mode(TG_GCODE_MODE);
	return (TG_OK);
}
*/

/* FURTHER NOTES

---- Generalized Serial Handler / Parser ----

  Want to do the following things:
	- Be able to interpret (and mix) various types of inputs, including:
		- Control commands from stdio - e.g. ^c, ^q/^p, ^n/^o...
		- Configuration commands for various sub-systems
		- Gcode interpreter blocks
		- Motion control commands (that bypass the Gcode layer)
		- Multi-DOF protocols TBD 
	- Accept and mix inputs from multiple sources:
		- USB
		- RS-485
		- Arduino serial port (Aux)
		- strings in program memory
		- EEPROM data
		- SD card data
	- Accept multiple types of line terminators including:
		- CR
		- LF
		- semicolon
		- NUL

---- Design notes ----
  	- line readers are the lowest level (above single character read)
		From serial inputs: read single characters to assemble a string
		From in-memory strings: read characters from a string in program memory
		Either mode: read string to next terminator and return NULL terminated string 
		Do not otherwise process or normalize the string
	- tg_parser is the top-level parser / dispatcher
		Examine the head of the string to determine how to dispatch
		Supported dispatches:
		- Gcode block
		- Gcode configuration line
		- Direct drive (motion control) command
		- Network command / config (not implemented)
	- Individual parsers/interpreters are called from tg_parser
		These can assume:
		- They will only receive a single line (multi-line inputs have been split)
		- Tyey perform line normalization required for that dispatch type
		- Can run the current command to completion before receiving another command

	- Flow control
		Flow control is provided by the called routine running to completion 
		without blocking. If blocking could occur (e.g. move buffer is full)
		the routine should return and provide a continuation in the main 
		controller loop. This necessitates some careful state handling.

---- Coding Continuations ----

	Continuations are used to manage points where the application would 
	ordinarily block. Call it application managed threading by way of an 
	inverted control loop. By coding using continuations the application 
	does not need an RTOS and is extremely responsive (there are no "ticks")

	Rules for writing a continuation task:
	  - A continuation is a pair of routines. The first is the main routine,
		the second the continuation. See mc_line() and mc_line_continue().

	  - The main routine is called first. It may have args. It performs 
	    all initial actions and sets up a static structure to hold data
		that is needed by successive calls to the continuation. The main
		routine usually ends by calling the continuation inside a return().

	  - The task is called initially by calling the main routine, then will 
	  	be called repeatedly from the main controller loop. The continuation 
		cannot have input args - all necessary data should be available in 
		the static struct.

	  - What happens in the continuation is managed by the continuation state
	  	which is typically  one of: OFF, NEW, or RUNNING. The state on initial 
		entry from the main routine should be NEW.

	  - The continuation must return the following codes, and may return 
	  	additional codes to indicate various exception conditions:

		  -	TG_NOOP: No operation ocurred. This is the usual return from an 
		  			 OFF state. All continuations must be callable with no 
					 effect when they are OFF (as they are called repeatedly 
					 whether or not they are active).

		  - TG_AGAIN: Continuation is blocked or still processing (RUNNING).

		  - TG_OK; 	 Continuation task  has just is completed - i.e. it has 
		  			 just transitioned to OFF. TG_OK should only be returned 
					 once. The next state will be OFF, which will return NOOP.
*/
