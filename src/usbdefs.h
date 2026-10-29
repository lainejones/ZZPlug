/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/* usbdefs.h - the USB 2.0 numbers ZZPlug needs (chapter 9 and 11). */
#ifndef ZZP_USBDEFS_H
#define ZZP_USBDEFS_H

/* bmRequestType */
#define RT_IN        0x80
#define RT_OUT       0x00
#define RT_STANDARD  0x00
#define RT_CLASS     0x20
#define RT_DEVICE    0x00
#define RT_INTERFACE 0x01
#define RT_OTHER     0x03       /* a hub port */

/* standard requests */
#define REQ_GET_STATUS        0
#define REQ_CLEAR_FEATURE     1
#define REQ_SET_FEATURE       3
#define REQ_SET_ADDRESS       5
#define REQ_GET_DESCRIPTOR    6
#define REQ_SET_CONFIGURATION 9

/* descriptor types */
#define DT_DEVICE     1
#define DT_CONFIG     2
#define DT_STRING     3
#define DT_INTERFACE  4
#define DT_ENDPOINT   5
#define DT_HUB        0x29

/* classes */
#define CLASS_PER_INTERFACE 0
#define CLASS_AUDIO   1
#define CLASS_COMM    2
#define CLASS_HID     3
#define CLASS_PRINTER 7
#define CLASS_STORAGE 8
#define CLASS_HUB     9
#define CLASS_CDC_DATA 0x0a
#define CLASS_VIDEO   0x0e
#define CLASS_VENDOR  0xff

/* hub port features (USB 2.0 table 11-17) */
#define PORT_CONNECTION   0
#define PORT_ENABLE       1
#define PORT_RESET        4
#define PORT_POWER        8
#define C_PORT_CONNECTION 16
#define C_PORT_ENABLE     17
#define C_PORT_RESET      20

/* hub port status bits (wPortStatus) */
#define PS_CONNECTION 0x0001
#define PS_ENABLE     0x0002
#define PS_RESET      0x0010
#define PS_POWER      0x0100
#define PS_LOW_SPEED  0x0200
#define PS_HIGH_SPEED 0x0400
/* wPortChange */
#define PC_CONNECTION 0x0001
#define PC_RESET      0x0010

/* speeds, as the ZZ9000 proxy numbers them */
#define SPEED_LOW  0
#define SPEED_FULL 1
#define SPEED_HIGH 2

#endif
