/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The few kernel interfaces tab5-kbd.c uses, for building the driver as a
 * host program (test-tab5-kbd.py).  Every <linux/...> header the driver
 * includes resolves to this file.  Keycodes come from the host's own
 * <linux/input-event-codes.h>, so they are the real ones.
 */
#ifndef TAB5_KBD_KSTUB_H
#define TAB5_KBD_KSTUB_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/input-event-codes.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

#define BIT(n)			(1UL << (n))
#define ARRAY_SIZE(a)		(sizeof(a) / sizeof((a)[0]))
#define __iomem
#define GFP_KERNEL		0
#define min_t(t, a, b)		((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define container_of(p, type, member) \
	((type *)((char *)(p) - offsetof(type, member)))

/* HZ is set on the compiler command line. */
extern unsigned long jiffies;
static inline unsigned long msecs_to_jiffies(unsigned int ms)
{
	return ((unsigned long)ms * HZ + 999) / 1000;
}
#define time_before(a, b)	((long)((a) - (b)) < 0)

struct device {
	void *of_node;
};

void stub_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define dev_info(dev, ...)		((void)(dev), stub_log(__VA_ARGS__))
#define dev_warn(dev, ...)		((void)(dev), stub_log(__VA_ARGS__))
#define dev_warn_ratelimited(dev, ...)	((void)(dev), stub_log(__VA_ARGS__))
#define dev_err_probe(dev, err, ...) \
	((void)(dev), stub_log(__VA_ARGS__), (err))

void *devm_kzalloc(struct device *dev, size_t size, int flags);
void *devm_ioremap(struct device *dev, unsigned long base, size_t size);
static inline u32 readl(const void *p)
{
	u32 v;

	memcpy(&v, p, sizeof(v));
	return v;
}
static inline void writel(u32 v, void *p)
{
	memcpy(p, &v, sizeof(v));
}

int of_property_read_u32(const void *np, const char *name, u32 *out);
struct of_device_id {
	const char *compatible;
};

#define MODULE_DEVICE_TABLE(type, name)	extern int stub_unused
#define MODULE_AUTHOR(s)		extern int stub_unused
#define MODULE_DESCRIPTION(s)		extern int stub_unused
#define MODULE_LICENSE(s)		extern int stub_unused

struct i2c_adapter {
	int unused;
};
struct i2c_client {
	u16 addr;
	struct i2c_adapter *adapter;
	struct device dev;
	void *clientdata;
};
struct i2c_msg {
	u16 addr;
	u16 flags;
	u16 len;
	u8 *buf;
};
#define I2C_M_RD		0x0001
#define I2C_FUNC_I2C		0x0001
static inline int i2c_check_functionality(struct i2c_adapter *a, u32 f)
{
	(void)a;
	(void)f;
	return 1;
}
int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
int i2c_master_send(const struct i2c_client *client, const void *buf,
		    int count);
static inline void i2c_set_clientdata(struct i2c_client *c, void *data)
{
	c->clientdata = data;
}
static inline void *i2c_get_clientdata(const struct i2c_client *c)
{
	return c->clientdata;
}
struct i2c_driver {
	struct {
		const char *name;
		const struct of_device_id *of_match_table;
	} driver;
	int (*probe)(struct i2c_client *client);
	void (*remove)(struct i2c_client *client);
};
#define module_i2c_driver(d)	struct i2c_driver *stub_driver = &(d)

#define BUS_I2C			0x18
struct input_dev {
	const char *name;
	const char *phys;
	struct {
		u16 bustype, vendor, product, version;
	} id;
	struct {
		struct device *parent;
	} dev;
	unsigned long evbit[1];
	unsigned long keybit[KEY_CNT / (8 * sizeof(unsigned long)) + 1];
};
static inline void __set_bit(unsigned int nr, unsigned long *addr)
{
	addr[nr / (8 * sizeof(unsigned long))] |=
		1UL << (nr % (8 * sizeof(unsigned long)));
}
static inline bool stub_test_bit(unsigned int nr, const unsigned long *addr)
{
	return addr[nr / (8 * sizeof(unsigned long))] >>
		(nr % (8 * sizeof(unsigned long))) & 1;
}
struct input_dev *devm_input_allocate_device(struct device *dev);
static inline int input_register_device(struct input_dev *input)
{
	(void)input;
	return 0;
}
void input_event(struct input_dev *input, unsigned int type,
		 unsigned int code, int value);
static inline void input_set_capability(struct input_dev *input,
					unsigned int type, unsigned int code)
{
	(void)code;
	__set_bit(type, input->evbit);
}
static inline void input_report_key(struct input_dev *input,
				    unsigned int code, int value)
{
	input_event(input, EV_KEY, code, !!value);
}
static inline void input_sync(struct input_dev *input)
{
	input_event(input, EV_SYN, SYN_REPORT, 0);
}

struct work_struct {
	void (*func)(struct work_struct *work);
};
struct delayed_work {
	struct work_struct work;
};
#define INIT_DELAYED_WORK(dw, fn)	((dw)->work.func = (fn))
static inline struct delayed_work *to_delayed_work(struct work_struct *work)
{
	return container_of(work, struct delayed_work, work);
}
bool schedule_delayed_work(struct delayed_work *dw, unsigned long delay);
static inline void cancel_delayed_work_sync(struct delayed_work *dw)
{
	(void)dw;
}

#endif
