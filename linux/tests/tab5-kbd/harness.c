// SPDX-License-Identifier: GPL-2.0-only
/*
 * tab5-kbd.c run as a host program against a model of the keyboard MCU.
 *
 * The model follows the MCU firmware's register handling (reset values,
 * which writes it ignores, how many bytes a read may ask for).  What the
 * driver reports is typed into a model of a US-layout console, so a
 * scenario reads as "these keys, in this order and timing, give this text".
 * The expected characters per key are written out here from the legends on
 * the keys, not derived from the driver's table.
 */
#include <stdarg.h>

#include "kstub.h"
#include "tab5-kbd.c"

/* Like the kernel's INITIAL_JIFFIES: the counter wraps during the run. */
unsigned long jiffies = (unsigned long)-(3 * HZ);

static const char *scenario = "start";
static int failures;

static void fail(const char *fmt, ...)
{
	va_list ap;

	printf("FAIL [%s]: ", scenario);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	failures++;
}

static char log_text[4096];

void stub_log(const char *fmt, ...)
{
	size_t used = strlen(log_text);
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(log_text + used, sizeof(log_text) - used, fmt, ap);
	va_end(ap);
}

/* ---------------------------------------------------------------- MCU */

static struct {
	bool attached;
	u8 cfg, sta, brightness, kb_mode, rgb_mode, version;
	u8 rgb[8];
	u8 fifo[64];
	unsigned int head, count;
	unsigned int transfers;
	unsigned int led0_changes;
} mcu;

static void mcu_reset(void)
{
	bool attached = mcu.attached;

	memset(&mcu, 0, sizeof(mcu));
	mcu.attached = attached;
	mcu.cfg = 0x07;
	mcu.brightness = 20;
	mcu.version = 0x01;
}

static void mcu_push(u8 raw)
{
	if (mcu.count == ARRAY_SIZE(mcu.fifo)) {
		fail("test pushed more events than the MCU queue holds");
		return;
	}
	mcu.fifo[(mcu.head + mcu.count++) % ARRAY_SIZE(mcu.fifo)] = raw;
}

/* The firmware's timer: flag queued key events when they are enabled. */
static void mcu_tick(void)
{
	if (mcu.kb_mode == 0 && (mcu.cfg & 0x01) && mcu.count)
		mcu.sta |= 0x01;
}

static int mcu_read(u8 reg, u8 *buf, unsigned int len)
{
	u8 block[4];
	unsigned int base, size;

	if (reg <= 0x03) {
		block[0] = mcu.cfg;
		block[1] = mcu.sta;
		block[2] = mcu.kb_mode == 0 ? mcu.count : 0;
		block[3] = mcu.brightness;
		base = 0x00;
		size = 4;
	} else if (reg >= 0x10 && reg <= 0x11) {
		block[0] = mcu.kb_mode;
		block[1] = mcu.rgb_mode;
		base = 0x10;
		size = 2;
	} else if (reg == 0x20) {
		block[0] = 0xff;
		if (mcu.count) {
			block[0] = mcu.fifo[mcu.head];
			mcu.head = (mcu.head + 1) % ARRAY_SIZE(mcu.fifo);
			mcu.count--;
		}
		base = 0x20;
		size = 1;
	} else if (reg >= 0x60 && reg <= 0x67) {
		if (len > 0x68u - reg) {
			fail("read of %u bytes at 0x%02x runs past the LEDs",
			     len, reg);
			return -1;
		}
		memcpy(buf, &mcu.rgb[reg - 0x60], len);
		return 0;
	} else if (reg == 0xfe) {
		block[0] = mcu.version;
		base = 0xfe;
		size = 1;
	} else {
		fail("read of unknown register 0x%02x", reg);
		return -1;
	}
	if (len > base + size - reg) {
		fail("read of %u bytes at 0x%02x runs past its block",
		     len, reg);
		return -1;
	}
	memcpy(buf, &block[reg - base], len);
	return 0;
}

static void mcu_write(u8 reg, const u8 *data, unsigned int len)
{
	unsigned int i;

	for (i = 0; i < len; i++, reg++) {
		u8 v = data[i];

		switch (reg) {
		case 0x00:
			mcu.cfg = v & 0x07;
			break;
		case 0x01:
			if (!v)
				mcu.sta = 0;
			break;
		case 0x02:
			if (!v && mcu.kb_mode == 0)
				mcu.head = mcu.count = 0;
			break;
		case 0x03:
			if (v <= 100)
				mcu.brightness = v;
			break;
		case 0x10:
			if (v != mcu.kb_mode && v <= 2) {
				mcu.head = mcu.count = 0;
				mcu.kb_mode = v;
			}
			break;
		case 0x11:
			if (v <= 1)
				mcu.rgb_mode = v;
			break;
		case 0x60 ... 0x67:
			if (reg == 0x60 && !v != !mcu.rgb[0])
				mcu.led0_changes++;
			mcu.rgb[reg - 0x60] = v;
			break;
		default:
			fail("write to unknown register 0x%02x", reg);
			return;
		}
	}
}

int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	(void)adap;
	mcu.transfers++;
	if (!mcu.attached)
		return -ENXIO;
	if (num != 2 || msgs[0].len != 1 || !(msgs[1].flags & I2C_M_RD)) {
		fail("transfer is not a register read");
		return -EIO;
	}
	return mcu_read(msgs[0].buf[0], msgs[1].buf, msgs[1].len) ? -EIO : num;
}

int i2c_master_send(const struct i2c_client *client, const void *buf,
		    int count)
{
	const u8 *b = buf;

	(void)client;
	mcu.transfers++;
	if (!mcu.attached)
		return -ENXIO;
	/* The firmware drops a longer write than its block allows. */
	if (count < 2 || (b[0] <= 0x03 && count > 5) ||
	    (b[0] >= 0x10 && b[0] <= 0x11 && count > 3) ||
	    (b[0] >= 0x60 && b[0] <= 0x67 && count > 9)) {
		fail("write of %d bytes at 0x%02x is dropped by the MCU",
		     count, b[0]);
		return count;
	}
	mcu_write(b[0], b + 1, count - 1);
	return count;
}

/* ------------------------------------------------------------ console */

static struct {
	bool down[KEY_CNT];
	bool unsynced;
	char out[1024];
} con;

static struct input_dev *the_input;

static const struct {
	unsigned int code;
	const char *plain, *shifted;
} con_keys[] = {
	{ KEY_A, "a", "A" }, { KEY_B, "b", "B" }, { KEY_C, "c", "C" },
	{ KEY_D, "d", "D" }, { KEY_E, "e", "E" }, { KEY_F, "f", "F" },
	{ KEY_G, "g", "G" }, { KEY_H, "h", "H" }, { KEY_I, "i", "I" },
	{ KEY_J, "j", "J" }, { KEY_K, "k", "K" }, { KEY_L, "l", "L" },
	{ KEY_M, "m", "M" }, { KEY_N, "n", "N" }, { KEY_O, "o", "O" },
	{ KEY_P, "p", "P" }, { KEY_Q, "q", "Q" }, { KEY_R, "r", "R" },
	{ KEY_S, "s", "S" }, { KEY_T, "t", "T" }, { KEY_U, "u", "U" },
	{ KEY_V, "v", "V" }, { KEY_W, "w", "W" }, { KEY_X, "x", "X" },
	{ KEY_Y, "y", "Y" }, { KEY_Z, "z", "Z" },
	{ KEY_1, "1", "!" }, { KEY_2, "2", "@" }, { KEY_3, "3", "#" },
	{ KEY_4, "4", "$" }, { KEY_5, "5", "%" }, { KEY_6, "6", "^" },
	{ KEY_7, "7", "&" }, { KEY_8, "8", "*" }, { KEY_9, "9", "(" },
	{ KEY_0, "0", ")" },
	{ KEY_MINUS, "-", "_" }, { KEY_EQUAL, "=", "+" },
	{ KEY_LEFTBRACE, "[", "{" }, { KEY_RIGHTBRACE, "]", "}" },
	{ KEY_BACKSLASH, "\\", "|" }, { KEY_SEMICOLON, ";", ":" },
	{ KEY_APOSTROPHE, "'", "\"" }, { KEY_GRAVE, "`", "~" },
	{ KEY_COMMA, ",", "<" }, { KEY_DOT, ".", ">" },
	{ KEY_SLASH, "/", "?" }, { KEY_SPACE, " ", "S-<space>" },
	{ KEY_ESC, "<esc>", "S-<esc>" }, { KEY_TAB, "<tab>", "S-<tab>" },
	{ KEY_ENTER, "<enter>", "S-<enter>" },
	{ KEY_BACKSPACE, "<bksp>", "S-<bksp>" },
	{ KEY_DELETE, "<del>", "S-<del>" },
	{ KEY_UP, "<up>", "S-<up>" }, { KEY_DOWN, "<down>", "S-<down>" },
	{ KEY_LEFT, "<left>", "S-<left>" },
	{ KEY_RIGHT, "<right>", "S-<right>" },
};

static void con_type(unsigned int code)
{
	unsigned int i;

	if (code == KEY_LEFTSHIFT || code == KEY_LEFTCTRL ||
	    code == KEY_LEFTALT)
		return;
	for (i = 0; i < ARRAY_SIZE(con_keys); i++) {
		if (con_keys[i].code != code)
			continue;
		if (con.down[KEY_LEFTCTRL])
			strcat(con.out, "^");
		if (con.down[KEY_LEFTALT])
			strcat(con.out, "M-");
		strcat(con.out, con.down[KEY_LEFTSHIFT] ?
		       con_keys[i].shifted : con_keys[i].plain);
		return;
	}
	fail("keycode %u is not a key of this keyboard", code);
}

void input_event(struct input_dev *input, unsigned int type,
		 unsigned int code, int value)
{
	if (type == EV_SYN) {
		con.unsynced = false;
		return;
	}
	con.unsynced = true;
	if (type != EV_KEY)
		return;
	/* The input core drops both of these without a word. */
	if (!stub_test_bit(code, input->keybit))
		fail("keycode %u was not announced in keybit", code);
	if (!!value == con.down[code])
		fail("keycode %u reported %s twice", code,
		     value ? "down" : "up");
	con.down[code] = value;
	if (value)
		con_type(code);
}

static void expect_out(const char *want)
{
	if (strcmp(con.out, want))
		fail("typed \"%s\", expected \"%s\"", con.out, want);
	con.out[0] = '\0';
}

static void expect_all_up(void)
{
	unsigned int code;

	for (code = 0; code < KEY_CNT; code++)
		if (con.down[code])
			fail("keycode %u is still down", code);
}

/* ------------------------------------------------------ kernel pieces */

static u8 iomux[0x100];
static int dt_brightness = -1;
static struct delayed_work *sched_dw;
static unsigned long sched_delay;
static unsigned int polls;

void *devm_kzalloc(struct device *dev, size_t size, int flags)
{
	(void)dev;
	(void)flags;
	return calloc(1, size);
}

void *devm_ioremap(struct device *dev, unsigned long base, size_t size)
{
	(void)dev;
	(void)base;
	return size <= sizeof(iomux) ? iomux : NULL;
}

struct input_dev *devm_input_allocate_device(struct device *dev)
{
	(void)dev;
	the_input = calloc(1, sizeof(*the_input));
	return the_input;
}

int of_property_read_u32(const void *np, const char *name, u32 *out)
{
	(void)np;
	if (strcmp(name, "m5stack,led-brightness") || dt_brightness < 0)
		return -EINVAL;
	*out = dt_brightness;
	return 0;
}

bool schedule_delayed_work(struct delayed_work *dw, unsigned long delay)
{
	if (sched_dw)
		return false;
	sched_dw = dw;
	sched_delay = delay;
	return true;
}

/* Let the given time pass, running the poll work whenever it is due. */
static void run_ms(unsigned int ms)
{
	unsigned long left = msecs_to_jiffies(ms);

	while (sched_dw && sched_delay <= left) {
		struct delayed_work *dw = sched_dw;

		jiffies += sched_delay;
		left -= sched_delay;
		sched_dw = NULL;
		mcu_tick();
		polls++;
		dw->work.func(&dw->work);
		if (con.unsynced) {
			fail("events were left without a sync");
			con.unsynced = false;
		}
	}
	jiffies += left;
	if (sched_dw)
		sched_delay -= left;
}

static struct i2c_adapter adapter;
static struct i2c_client client;

static void boot(const char *name, bool attached, int brightness)
{
	scenario = name;
	memset(&con, 0, sizeof(con));
	log_text[0] = '\0';
	mcu.attached = attached;
	mcu_reset();
	dt_brightness = brightness;
	sched_dw = NULL;
	memset(&client, 0, sizeof(client));
	client.addr = 0x6d;
	client.adapter = &adapter;
	if (stub_driver->probe(&client))
		fail("probe failed");
	run_ms(60);
}

/* --------------------------------------------------------------- keys */

/* The legends on the keys, by the MCU's logical (row, col). */
static const char *const legend[TAB5KBD_ROWS][TAB5KBD_COLS] = {
	{ "esc", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "+",
	  "del" },
	{ "`", "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "[", "]",
	  "\\" },
	{ "tab", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", ";", "'",
	  "bksp" },
	{ "sym", "Aa", "a", "s", "d", "f", "g", "h", "j", "k", "l", "up", "_",
	  "enter" },
	{ "ctrl", "alt", "z", "x", "c", "v", "b", "n", "m", ".", "left",
	  "down", "right", "space" },
};

/* The second legend of the keys that have one; typed with sym held. */
static const char *const sym_legend[TAB5KBD_ROWS][TAB5KBD_COLS] = {
	[1] = { "~", "?", NULL, NULL, NULL, NULL, NULL, NULL, "/", "<", ">",
		"{", "}", "|" },
	[2] = { [11] = ":", [12] = "\"" },
	[3] = { [12] = "=" },
	[4] = { [9] = "," },
};

static u8 pos_of(const char *name)
{
	unsigned int row, col;

	for (row = 0; row < TAB5KBD_ROWS; row++)
		for (col = 0; col < TAB5KBD_COLS; col++)
			if (!strcmp(legend[row][col], name))
				return (row << 4) | col;
	fail("no key is labelled \"%s\"", name);
	return 0;
}

static void press(const char *name)
{
	mcu_push(0x80 | pos_of(name));
	run_ms(30);
}

static void release(const char *name)
{
	mcu_push(pos_of(name));
	run_ms(30);
}

static void tap(const char *name)
{
	press(name);
	release(name);
}

/* Tap the space-separated keys one after another. */
static void taps(const char *names)
{
	char buf[256], *name;

	snprintf(buf, sizeof(buf), "%s", names);
	for (name = strtok(buf, " "); name; name = strtok(NULL, " "))
		tap(name);
}

/* What a key's legend looks like once typed. */
static void typed(char *out, const char *name)
{
	if (strlen(name) == 1)
		strcat(out, name);
	else if (!strcmp(name, "space"))
		strcat(out, " ");
	else
		sprintf(out + strlen(out), "<%s>", name);
}

/* ---------------------------------------------------------- scenarios */

static void test_every_key(void)
{
	char want[512];
	unsigned int row, col;

	boot("every key gives its legend", true, 20);
	for (row = 0; row < TAB5KBD_ROWS; row++) {
		want[0] = '\0';
		for (col = 0; col < TAB5KBD_COLS; col++) {
			const char *name = legend[row][col];

			if (!strcmp(name, "sym") || !strcmp(name, "Aa") ||
			    !strcmp(name, "ctrl") || !strcmp(name, "alt"))
				continue;
			tap(name);
			typed(want, name);
		}
		expect_out(want);
	}

	scenario = "every key gives its second legend with sym held";
	press("sym");
	for (row = 0; row < TAB5KBD_ROWS; row++) {
		want[0] = '\0';
		for (col = 0; col < TAB5KBD_COLS; col++) {
			const char *name = legend[row][col];

			if (!strcmp(name, "sym") || !strcmp(name, "Aa") ||
			    !strcmp(name, "ctrl") || !strcmp(name, "alt"))
				continue;
			tap(name);
			typed(want, sym_legend[row][col] ?
			      sym_legend[row][col] : name);
		}
		expect_out(want);
	}
	release("sym");
	expect_out("");
	expect_all_up();
}

static void test_aa(void)
{
	unsigned int changes;

	boot("one Aa tap makes the next letter upper case", true, 20);
	tap("Aa");
	taps("h e l l o");
	expect_out("Hello");

	scenario = "a digit does not use up the Aa tap";
	run_ms(1000);
	tap("Aa");
	taps("1 a b");
	expect_out("1Ab");

	scenario = "the LED blinks while a tap is pending";
	run_ms(1000);
	changes = mcu.led0_changes;
	tap("Aa");
	if (!mcu.rgb[0])
		fail("the LED is dark right after the tap");
	run_ms(1000);
	/* 200 ms on, 200 ms off: five changes a second, give or take. */
	if (mcu.led0_changes - changes < 4 || mcu.led0_changes - changes > 7)
		fail("LED changed %u times in a second of blinking",
		     mcu.led0_changes - changes);
	tap("a");
	run_ms(100);
	if (mcu.rgb[0])
		fail("the LED is still lit after the letter");
	expect_out("A");

	scenario = "two Aa taps lock upper case until the next tap";
	run_ms(1000);
	tap("Aa");
	tap("Aa");
	run_ms(100);
	changes = mcu.led0_changes;
	taps("a b 1 c");
	run_ms(1000);
	if (!mcu.rgb[0] || mcu.led0_changes != changes)
		fail("the LED is not steadily lit while locked");
	tap("Aa");
	run_ms(1000);
	if (mcu.rgb[0])
		fail("the LED is still lit after unlocking");
	taps("d e");
	expect_out("AB1Cde");

	scenario = "Aa held is a Shift key and leaves nothing on";
	press("Aa");
	run_ms(600);
	taps("a b");
	release("Aa");
	taps("c");
	expect_out("ABc");

	scenario = "a short Aa hold with a letter leaves nothing on";
	run_ms(1000);
	press("Aa");
	tap("a");
	release("Aa");
	taps("b");
	run_ms(100);
	if (mcu.rgb[0])
		fail("the LED is lit after Aa was used as Shift");
	expect_out("Ab");

	scenario = "Ctrl and Alt letters stay plain while locked";
	run_ms(1000);
	tap("Aa");
	tap("Aa");
	press("ctrl");
	tap("c");
	release("ctrl");
	press("alt");
	tap("x");
	release("alt");
	tap("a");
	tap("Aa");
	expect_out("^cM-xA");
	expect_all_up();
}

static void test_overlap(void)
{
	boot("keys typed overlapping come out as typed", true, 20);
	press("(");
	press("f");
	release("(");
	release("f");
	expect_out("(f");

	press("f");
	press("(");
	release("f");
	release("(");
	expect_out("f(");

	press("!");
	press("1");
	press("@");
	release("!");
	release("@");
	release("1");
	expect_out("!1@");

	scenario = "sym let go first does not change the key's release";
	press("sym");
	press("(");
	release("sym");
	release("(");
	tap("(");
	expect_out("<(");

	scenario = "Ctrl reaches the keys that have a sym character";
	press("ctrl");
	tap("[");
	tap("c");
	release("ctrl");
	expect_out("^[^c");
	expect_all_up();
}

static void test_presence(void)
{
	boot("a keyboard attached after boot starts working", false, 20);
	if (!strstr(log_text, "absent"))
		fail("no absent message");
	run_ms(3000);
	mcu.attached = true;
	run_ms(1100);
	if (mcu.cfg != 0x01 || mcu.rgb_mode != 1 || mcu.kb_mode != 0)
		fail("not configured after attach: cfg %02x rgb %u mode %u",
		     mcu.cfg, mcu.rgb_mode, mcu.kb_mode);
	taps("a");
	expect_out("a");

	scenario = "a detached keyboard leaves no key held";
	tap("Aa");
	tap("Aa");
	press("b");
	press("(");
	mcu.attached = false;
	run_ms(500);
	expect_out("B(");
	expect_all_up();
	if (!strstr(log_text, "detached"))
		fail("no detached message");

	scenario = "re-attached: configured again, upper case off";
	mcu_reset();
	mcu.attached = true;
	run_ms(1100);
	taps("a");
	expect_out("a");
	if (mcu.cfg != 0x01 || mcu.rgb_mode != 1 || mcu.brightness != 20)
		fail("not configured after re-attach");

	scenario = "an MCU that restarted is configured again";
	log_text[0] = '\0';
	press("a");
	mcu_reset();
	run_ms(100);
	expect_out("a");
	expect_all_up();
	if (mcu.cfg != 0x01 || mcu.rgb_mode != 1)
		fail("not configured after an MCU restart");
	if (!strstr(log_text, "restarted"))
		fail("no restart message");
	taps("b");
	expect_out("b");
}

static void test_leds_and_bus(void)
{
	unsigned int before, i;

	boot("brightness comes from the device tree", true, 35);
	if (mcu.brightness != 35)
		fail("brightness is %u, expected 35", mcu.brightness);
	boot("a brightness over the MCU's limit is clamped", true, 250);
	if (mcu.brightness != 100)
		fail("brightness is %u, expected 100", mcu.brightness);
	boot("no brightness property keeps the MCU's level", true, -1);
	if (mcu.brightness != 20)
		fail("brightness is %u, expected 20", mcu.brightness);

	scenario = "only LED 0 is ever lit, and only in blue";
	tap("Aa");
	tap("Aa");
	run_ms(300);
	if (mcu.rgb[0] != 0xff)
		fail("LED 0 is not lit while locked");
	for (i = 1; i < ARRAY_SIZE(mcu.rgb); i++)
		if (mcu.rgb[i])
			fail("LED register 0x%02x is %02x", 0x60 + i,
			     mcu.rgb[i]);
	tap("Aa");

	scenario = "an idle poll is one transfer";
	run_ms(1000);
	before = mcu.transfers;
	polls = 0;
	run_ms(1000);
	if (mcu.transfers - before != polls || polls < 30)
		fail("%u transfers in %u idle polls", mcu.transfers - before,
		     polls);

	scenario = "odd events are ignored";
	mcu_push(0x80 | 0x5e);	/* row 5 */
	mcu_push(0x80 | 0x0f);	/* col 15 */
	press("a");
	mcu_push(0x80 | pos_of("a"));	/* a second press, release lost */
	release("a");
	release("a");
	expect_out("a");
	expect_all_up();
}

int main(void)
{
	test_every_key();
	test_aa();
	test_overlap();
	test_presence();
	test_leds_and_bus();
	if (failures) {
		printf("%d failure(s) at HZ=%d\n", failures, HZ);
		return 1;
	}
	return 0;
}
