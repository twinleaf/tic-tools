# Twinleaf I/O C Tools

This package implements a communications protocol in python to work with [Twinleaf sensors](http://www.twinleaf.com) using [Twinleaf I/O (TIO)](https://github.com/twinleaf/libtio/blob/master/doc/TIO%20Protocol%20Overview.md) as the communications layer. Data from the sensors is received via PUB messages and sensor parameters may be changed using REQ/REP messages. 

## Tools included

### proxy

The proxy is run on a serial port and serves TCP access to the sensor. Multiple clients may connect to port 7855 and recieve data. The usage is simply:

  $ bin/proxy /dev/cu.ttyUSB0

where the serial device varies across platforms. The [labview](https://github.com/twinleaf/tio-labview) and [python](https://github.com/twinleaf/tio-python) clients may simultaneously connect to the proxy server. 

### rpc_req

If the proxy is already running, simple RPC requests can be performed on the command line or from shell scripts using the rpc_req command:

  $ bin/rpc_req gmr.data.decimation
  04 00 00 00 
  u32: 0x00000004 4
  s32: 4
  f32: 0.000000

The user must know the data type to send a valid RPC request:

  $ bin/rpc_req gmr.data.decimation u32:10
  $ bin/rpc_req gmr.data.autocutoff u8:0
  $ bin/rpc_req gmr.data.cutoff f32:30

This set of commands changed decimation to 10 points, turned off the automatic antialias filter and changed the filter cutoff to 30 Hz. 

## Prerequisites

[libtio](https://github.com/twinleaf/libtio) is an included submodule. Be sure to run `git submodule update --init` after cloning this repository. The tools compile and run with no dependencies on standard POSIX systems:

  - Linux
  - macOS
  - Windows subsystem for linux

## Programming

A sockets interface to the sensors is provided. Please review examples and the code to understand its use.

## TODO

  - [ ] proxy automatically changes device into binary mode