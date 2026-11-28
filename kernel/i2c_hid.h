#ifndef NOVUS_I2C_HID_H
#define NOVUS_I2C_HID_H

/* HID-over-I2C pointing device on one of the PCH's LPSS I2C controllers.
 * init() probes every serial-bus controller for a HID device at the address
 * the firmware's _CRS named (falling back to the usual Synaptics/ELAN
 * addresses), reads the report descriptor and starts polling it for absolute
 * reports. poll() is safe to call from any input tick after init(). */
int  i2c_hid_init(void);
void i2c_hid_poll(void);

/* Answers 1 when a device answered and its reports are being polled, 0 when
 * no I2C-HID device is in use, -1 before init() ran. */
int  i2c_hid_state(void);

/* How many serial-bus (LPSS) controllers the PCI scan found, whether or not
 * a touchpad answered on them. One number that tells a machine where the
 * firmware never exposed a bus at all apart from one where the bus exists
 * but the device is parked in reset or unpowered. */
int  i2c_hid_buses(void);

#endif /* NOVUS_I2C_HID_H */
