#ifndef _RDC321X_GPIO_H
#define _RDC321X_GPIO_H

extern int rdc_gpio_get_value(unsigned gpio);
extern void rdc_gpio_set_value(unsigned gpio, int value);
extern int rdc_gpio_direction_input(unsigned gpio);
extern int rdc_gpio_direction_output(unsigned gpio, int value);


/* Wrappers for the arch-neutral GPIO API */

static inline int gpio_request(unsigned gpio, const char *label)
{
	/* Not yet implemented */
	return 0;
}

static inline void gpio_free(unsigned gpio)
{
	/* Not yet implemented */
}

static inline int gpio_direction_input(unsigned gpio)
{
	return rdc_gpio_direction_input(gpio);
}

static inline int gpio_direction_output(unsigned gpio, int value)
{
	return rdc_gpio_direction_output(gpio, value);
}

static inline int gpio_get_value(unsigned gpio)
{
	return rdc_gpio_get_value(gpio);
}

static inline void gpio_set_value(unsigned gpio, int value)
{
	rdc_gpio_set_value(gpio, value);
}

static inline int gpio_to_irq(unsigned gpio)
{
	return gpio;
}

static inline int irq_to_gpio(unsigned irq)
{
	return irq;
}

/* For cansleep */
#include <asm-generic/gpio.h>

#endif /* _RDC321X_GPIO_H_ */
