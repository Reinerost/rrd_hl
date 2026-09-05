# rrd_hl

`rrd_hl` is a lightweight, headless version of RRDtool for embedded
systems. It is based on RRDtool 1.11.0 and removes graphics and other
dependencies that are not required for headless data processing.

The primary target is OpenWrt and other resource-constrained Linux
systems.

## Features

`rrd_hl` retains the RRDtool core functionality required for creating,
updating, fetching and exporting RRD data, including:

- RRD create, update, fetch and dump
- DEF
- CDEF / RPN expressions
- VDEF
- SHIFT
- XPORT
- XML, JSON and CSV export

The graph data processing required by XPORT is retained without the
graphics rendering layer.

## Removed components

To reduce size and external dependencies, `rrd_hl` does not build:

- graph rendering
- Cairo / Pango based graphics
- rrdcached
- restore support
- language bindings

The headless implementation also removes the runtime dependencies on
GLib, PCRE and libxml2.

## Origin

`rrd_hl` is based on RRDtool 1.11.0.

RRDtool is developed by Tobi Oetiker and contributors.
The original copyright and license information is retained in this
repository.

For the original RRDtool project, see:
https://github.com/oetiker/rrdtool-1.x
