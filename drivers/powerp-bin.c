/* powerp-bin.c	Model specific routines for CyberPower binary
			protocol UPSes 

   Copyright (C)
	2007		Doug Reynolds <mav@wastegate.net>
	2007-2008	Arjen de Korte <adkorte-guest@alioth.debian.org>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

/*
   Throughout this driver, READ and WRITE comments are shown. These are
   the typical commands to and replies from the UPS that was used for
   decoding the protocol (with a serial logger).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "main.h"
#include "serial.h"

#include "powerpanel.h"
#include "powerp-bin.h"

typedef struct {
	unsigned char	start;
	unsigned char	i_volt;
	unsigned char	o_volt;
	unsigned char	o_load;
	unsigned char	fill_4;
	unsigned char	b_chrg;
	unsigned char	u_temp;
	unsigned char	i_freq;
	unsigned char	fill_8;
	unsigned char	flags[4];
	unsigned char	stop;
} status_t;

typedef struct {
	char	*val;
	char	command;
} valtab_t;

static enum {
	PR = 0,
	OP = 1
} type = PR;

static unsigned char	powpan_answer[SMALLBUF];

/* default */
static const valtab_t	tran_high[] = {
	{ "138", -9 }, { "139", -8 }, { "140", -7 }, { "141", -6 }, { "142", -5 },
	{ "143", -4 }, { "144", -3 }, { "145", -2 }, { "146", -1 }, { "147",  0 },
	{ NULL, 0 }
};

/* OP series */
static const valtab_t	tran_high_op[] = {
	{ "140", -5 }, { "141", -4 }, { "142", -3 }, { "143", -2 }, { "144", -1 },
	{ "145",  0 }, { "146", +1 }, { "147", +2 }, { "148", +3 }, { "149", +4 },
	{ "150", +5 }, { NULL, 0 }
};

/* default */
static const valtab_t	tran_low[] = {
	{ "88",  0 }, { "89", +1 }, { "90", +2 }, { "91", +3 }, { "92", +4 },
	{ "93", +5 }, { "94", +6 }, { "95", +7 }, { "96", +8 }, { "97", +9 },
	{ NULL, 0 }
};

/* OP series */
static const valtab_t	tran_low_op[] = {
	{ "85", -5 }, { "86", -4 }, { "87", -3 }, { "88", -2 }, { "89", -1 },
	{ "90",  0 }, { "91", +1 }, { "92", +2 }, { "93", +3 }, { "94", +4 },
	{ "95", +5 }, { NULL, 0 }
};

/* default */
static const valtab_t	batt_low[] = {
	{ "25", -6 }, { "30", -5 }, { "35", -3 }, { "40", -1 }, { "45",  0 },
	{ "50", +2 }, { "55", +4 }, { "60", +6 }, { NULL, 0 }
};

/* OP series */
static const valtab_t	batt_low_op[] = {
	{ "15", -8 }, { "18", -7 }, { "19", -6 }, { "20", -5 }, { "22", -4 },
	{ "24", -3 }, { "25", -2 }, { "26", -1 }, { "28",  0 }, { "30", +1 },
	{ "32", +2 }, { "34", +3 }, { "35", +4 }, { "36", +5 }, { "38", +6 },
	{ "40", +7 }, { NULL, 0 }
};

/* default */
static const valtab_t	out_volt[] = {
	{ "110", -10 }, { "120",  0 }, { "130", +10 }, { NULL, 0 }
};

/* OP series */
static const valtab_t	out_volt_op[] = {
	{ "110", -10 }, { "115", -5 }, { "120",  0 }, { "124", +4 }, { "128", +8 },
	{ "130", +10 }, { NULL, 0 }
};

static const valtab_t 	yes_no_info[] = {
	{ "yes", 2 }, { "no", 0 },
	{ NULL, 0 }
};

static const struct {
	char	*var;
	char	*get;
	char	*set;
	const valtab_t	*map;
} vartab[][6] = {
	{
		/* default */
		{ "input.transfer.high", "R\002\r", "Q\002%c\r", tran_high },
		{ "input.transfer.low", "R\004\r", "Q\004%c\r", tran_low },
		{ "battery.charge.low", "R\010\r", "Q\010%c\r", batt_low },
		{ "output.voltage.nominal", "R\030\r", "Q\030%c\r", out_volt },
		{ "ups.start.battery", "R\017\r", "Q\017%c\r", yes_no_info },
		{ NULL, NULL, NULL, NULL }
	}, {
		/* OP series */
		{ "input.transfer.high", "R\002\r", "Q\002%c\r", tran_high_op },
		{ "input.transfer.low", "R\004\r", "Q\004%c\r", tran_low_op },
		{ "battery.charge.low", "R\010\r", "Q\010%c\r", batt_low_op },
		{ "output.voltage.nominal", "R\030\r", "Q\030%c\r", out_volt_op },
		{ NULL, NULL, NULL, NULL }
	}	
};

static const struct {
	char	*cmd;
	char	*command;
	int	len;
} cmdtab[] = {
	{ "test.failure.start", "T\230\r", 3 },		/* 20 seconds test */
	{ "test.failure.stop", "CT\r", 3 },
	{ "beeper.toggle", "B\r", 2 },
	{ "shutdown.reboot", "S\0\0R\0\1W\r", 8},
	{ "shutdown.return", "S\0\0W\r", 5 },
	{ "shutdown.stop", "C\r", 2 },
/*
	{ "shutdown.stayoff", "S\0\0\W\r", 5 },
 */
	{ NULL, NULL, 0 }
};

/* map UPS data to (approximated) input/output voltage */
static int op_volt(unsigned char in)
{
	if (in < 27) {
		return 0;
	}

	return (((float)in * 200 / 230) + 6);
}

/* map UPS data to (approximated) charge percentage */
static int op_chrg(unsigned char in)
{
	if (in > 185) {
		return 100;
	}

	if (in < 160) {
		return 0;
	}

	return pow((float)(in - 160)/2.56, 2);
}

/* map UPS data to (approximated) temperature */
static float op_temp(unsigned char in)
{
	return (pow((float)in / 32, 2) + 10);
}

/* map UPS data to (approximated) frequency */
static float op_freq(unsigned char in)
{
	return (12600.0 / (in + 32));
}

static int powpan_command(const char *buf, size_t bufsize)
{
	int	ret;

	ser_flush_io(upsfd);

	ret = ser_send_buf_pace(upsfd, UPSDELAY, buf, bufsize);

	if (ret < 0) {
		upsdebug_with_errno(3, "send");
		return -1;
	}

	if (ret == 0) {
		upsdebug_with_errno(3, "send: timeout");
		return -1;
	}

	upsdebug_hex(3, "send", buf, bufsize);

	usleep(100000);

	ret = ser_get_buf_len(upsfd, powpan_answer, bufsize-1, SER_WAIT_SEC, SER_WAIT_USEC);

	if (ret < 0) {
		upsdebug_with_errno(3, "read");
		upsdebug_hex(4, "  \\_", buf, bufsize-1);
		return -1;
	}

	if (ret == 0) {
		upsdebugx(3, "read: timeout");
		upsdebug_hex(4, "  \\_", buf, bufsize-1);
		return -1;
	}

	upsdebug_hex(3, "read", powpan_answer, ret);
	return ret;
}

static int powpan_instcmd(const char *cmdname, const char *extra)
{
	int	i;

	for (i = 0; cmdtab[i].cmd != NULL; i++) {

		if (strcasecmp(cmdname, cmdtab[i].cmd)) {
			continue;
		}

		if ((powpan_command(cmdtab[i].command, cmdtab[i].len) ==
				cmdtab[i].len - 1) &&
				(!memcmp(powpan_answer, cmdtab[i].command, cmdtab[i].len - 1))) {
			return STAT_INSTCMD_HANDLED;
		}

		upslogx(LOG_ERR, "powpan_instcmd: command [%s] failed", cmdname);
		return STAT_INSTCMD_UNKNOWN;
	}

	upslogx(LOG_NOTICE, "powpan_instcmd: command [%s] unknown", cmdname);
	return STAT_INSTCMD_UNKNOWN;
}

static int powpan_setvar(const char *varname, const char *val)
{
	char	command[SMALLBUF];
	int 	i, j;

	for (i = 0;  vartab[type][i].var != NULL; i++) {

		if (strcasecmp(varname, vartab[type][i].var)) {
			continue;
		}

		if (!strcasecmp(val, dstate_getinfo(varname))) {
			upslogx(LOG_INFO, "powpan_setvar: [%s] no change for variable [%s]", val, varname);
			return STAT_SET_HANDLED;
		}

		for (j = 0; vartab[type][i].map[j].val != NULL; j++) {

			if (strcasecmp(val, vartab[type][i].map[j].val)) {
				continue;
			}

			snprintf(command, sizeof(command), vartab[type][i].set,
				vartab[type][i].map[j].command);

			if ((powpan_command(command, 4) == 3) && (!memcmp(powpan_answer, command, 3))) {
				dstate_setinfo(varname, val);
				return STAT_SET_HANDLED;
			}

			upslogx(LOG_ERR, "powpan_setvar: setting variable [%s] to [%s] failed", varname, val);
			return STAT_SET_UNKNOWN;
		}

		upslogx(LOG_ERR, "powpan_setvar: [%s] is not valid for variable [%s]", val, varname);
		return STAT_SET_UNKNOWN;
	}

	upslogx(LOG_ERR, "powpan_setvar: variable [%s] not found", varname);
	return STAT_SET_UNKNOWN;
}

static void powpan_initinfo()
{
	int	i, j;
	char	*s;

	/*
	 * NOTE: The reply is already in the buffer, since the F\r command
	 * was used for autodetection of the UPS. No need to do it again.
	 */
	if ((s = strtok((char *)&powpan_answer[1], ".")) != NULL) {
		dstate_setinfo("ups.model", "%s", rtrim(s, ' '));
	}
	if ((s = strtok(NULL, ".")) != NULL) {
		dstate_setinfo("input.voltage.nominal", "%d", (unsigned char)s[0]);
	}
	if ((s = strtok(NULL, ".")) != NULL) {
		dstate_setinfo("input.frequency.nominal", "%d", (unsigned char)s[0]);
	}
	if ((s = strtok(NULL, ".")) != NULL) {
		dstate_setinfo("ups.firmware", "%c.%c%c%c", s[0], s[1], s[2], s[3]);
	}

	for (i = 0; cmdtab[i].cmd != NULL; i++) {
		dstate_addcmd(cmdtab[i].cmd);
	}

	for (i = 0; vartab[type][i].var != NULL; i++) {
		
		if (powpan_command(vartab[type][i].get, 3) < 2) {
			continue;
		}

		for (j = 0; vartab[type][i].map[j].val != NULL; j++) {

			if (vartab[type][i].map[j].command != powpan_answer[1]) {
				continue;
			}

			dstate_setinfo(vartab[type][i].var, vartab[type][i].map[j].val);
			break;
		}
	
		if (dstate_getinfo(vartab[type][i].var) == NULL) {
			upslogx(LOG_WARNING, "warning: [%d] unknown value for [%s]!",
				powpan_answer[1], vartab[type][i].var);
			continue;
		}

		dstate_setflags(vartab[type][i].var, ST_FLAG_RW);

		for (j = 0; vartab[type][i].map[j].val != 0; j++) {
			dstate_addenum(vartab[type][i].var, vartab[type][i].map[j].val);
		}
	}

	upsh.instcmd = powpan_instcmd;
	upsh.setvar = powpan_setvar;
}

static int powpan_status(status_t *status)
{
	int	ret;

	ser_flush_io(upsfd);

	/*
	 * WRITE D\r
	 * READ #VVL.CTF.....\r
        *      01234567890123
	 */
	ret = ser_send_pace(upsfd, UPSDELAY, "D\r");

	if (ret < 0) {
		upsdebug_with_errno(3, "send");
		return -1;
	}

	if (ret == 0) {
		upsdebug_with_errno(3, "send: timeout");
		return -1;
	}

	upsdebug_hex(3, "send", "D\r", 2);

	usleep(200000);

	ret = ser_get_buf_len(upsfd, status, sizeof(*status), SER_WAIT_SEC, SER_WAIT_USEC);

	if (ret < 0) {
		upsdebug_with_errno(3, "read");
		upsdebug_hex(4, "  \\_", status, sizeof(*status));
		return -1;
	}

	if (ret == 0) {
		upsdebugx(3, "read: timeout");
		upsdebug_hex(4, "  \\_", status, sizeof(*status));
		return -1;
	}

	upsdebug_hex(3, "read", status, ret);

	if ((status->flags[0] + status->flags[1]) != 255) {
		upsdebugx(4, "  \\_ : checksum flags[0..1] failed");
		return -1;
	}

	if ((status->flags[2] + status->flags[3]) != 255) {
		upsdebugx(4, "  \\_ : checksum flags[2..3] failed");
		return -1;
	}

	return 0;
}

static int powpan_updateinfo()
{
	status_t	status;

	if (powpan_status(&status)) {
		return -1;
	}

	switch (type)
	{
	case OP:
		dstate_setinfo("input.voltage", "%d", op_volt(status.i_volt));
		dstate_setinfo("output.voltage", "%d", op_volt(status.o_volt));
		dstate_setinfo("ups.load", "%d", status.o_load * 2);
		dstate_setinfo("battery.charge", "%d", op_chrg(status.b_chrg));
		dstate_setinfo("ups.temperature", "%.1f", op_temp(status.u_temp));
		dstate_setinfo("input.frequency", "%.1f", op_freq(status.i_freq));
		break;

	default:
		dstate_setinfo("input.voltage", "%d", status.i_volt);
		dstate_setinfo("output.voltage", "%d", status.o_volt);
		dstate_setinfo("ups.load", "%d", status.o_load);
		dstate_setinfo("battery.charge", "%d", status.b_chrg);
		dstate_setinfo("ups.temperature", "%d", status.u_temp);
		/*
		 * The following is just a wild guess. With a nominal input
		 * frequency of 60 Hz, the PR2200 shows a value of 150 (decimal).
		 * No idea what it means though, since we got only one reading.
		 */
		dstate_setinfo("input.frequency", "%.1f", (float)status.i_freq / 2.5);
		break;
	}

	if (status.flags[0] & 0x01) {
		dstate_setinfo("ups.beeper.status", "enabled");
	} else {
		dstate_setinfo("ups.beeper.status", "disabled");
	}

	status_init();

	if (status.flags[0] & 0x80) {
		status_set("OB");
	} else {
		status_set("OL");
	}

	if (status.flags[0] & 0x40) {
		status_set("LB");
	}

	/* !OB && !TEST */
	if (!(status.flags[0] & 0x84)) {
		if ((status.i_volt > 0.50 * status.o_volt) && (status.i_volt < 0.98 * status.o_volt)) {
			status_set("BOOST");
		}

		if ((status.o_volt > 0.50 * status.i_volt) && (status.o_volt < 0.98 * status.i_volt)) {
			status_set("TRIM");
		}
	}

	if (status.flags[0] & 0x04) {
		status_set("TEST");
	}

	if (status.flags[0] == 0) {
		status_set("OFF");
	}

	status_commit();

	return 0;
}

static void powpan_shutdown()
{
	status_t	status;
	int		i;

	for (i = 0; i < MAXTRIES; i++) {

		if (powpan_status(&status)) {
			continue;
		}

		/*
		 * We're still on battery...
		 */
		if (status.flags[0] & 0x80) {
			break;
		}
 
		/*
		 * Apparently, the power came back already, so just reboot.
		 */
		if (powpan_instcmd("shutdown.reboot", NULL) == STAT_INSTCMD_HANDLED) {
			upslogx(LOG_INFO, "Rebooting now...");
			return;
		}
	}

	for (i = 0; i < MAXTRIES; i++) {

		/*
		 * ...send wait for return.
		 */
		if (powpan_instcmd("shutdown.return", NULL) == STAT_INSTCMD_HANDLED) {
			upslogx(LOG_INFO, "Waiting for power to return...");
			return;
		}
	}

	upslogx(LOG_ERR, "Shutdown command failed!");
}

static int powpan_initups()
{
	int	ret, i;

	upsdebugx(1, "Trying binary protocol...");

	ser_set_speed(upsfd, device_path, B1200);

	/* This fails for many devices, so don't bother to complain */
	ser_send_pace(upsfd, UPSDELAY, "\r\r");

	for (i = 0; i < MAXTRIES; i++) {

		ser_flush_io(upsfd);

		/*
		 * WRITE F\r
		 * READ .PR2200    .x.<.1100
		 *      01234567890123456789
		 */
		ret = ser_send_pace(upsfd, UPSDELAY, "F\r");

		if (ret < 0) {
			upsdebug_with_errno(3, "send");
			continue;
		}

		if (ret == 0) {
			upsdebug_with_errno(3, "send: timeout");
			continue;
		}

		upsdebug_hex(3, "send", "F\r", 2);

		usleep(200000);

		ret = ser_get_line(upsfd, powpan_answer, sizeof(powpan_answer),
			ENDCHAR, IGNCHAR, SER_WAIT_SEC, SER_WAIT_USEC);

		if (ret < 0) {
			upsdebug_with_errno(3, "read");
			upsdebug_hex(4, "  \\_", powpan_answer, strlen((char *)powpan_answer));
			continue;
		}

		if (ret == 0) {
			upsdebugx(3, "read: timeout");
			upsdebug_hex(4, "  \\_", powpan_answer, strlen((char *)powpan_answer));
			continue;
		}

		upsdebug_hex(3, "read", powpan_answer, ret);

		if (ret < 20) {
			upsdebugx(2, "Expected 20 bytes but only got %d", ret);
			continue;
		}

		if (powpan_answer[0] != '.') {
			upsdebugx(2, "Expected start character '.' but got '%c'", (char)powpan_answer[0]);
			continue;
		}

		/* See if we need to use the 'old' protocol for the OP series */
		if (!strncmp((char *)&powpan_answer[1], "OP", 2)) {
			type = OP;
		}

		return ret;
	}

	return -1;
}

subdriver_t powpan_binary = {
	"binary",
	powpan_initups,
	powpan_initinfo,
	powpan_updateinfo,
	powpan_shutdown
};
