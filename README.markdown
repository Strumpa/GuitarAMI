
libmapper
=========

This library is a system for representing input and output signals on
a network and allowing arbitrary "mappings" to be dynamically created
between them using an external interface.

To get started quickly with libmapper, be sure to read the tutorial,
found in the "doc" folder in this distribution.

This project began life as a tool for helping composers and performers
to more easily play with custom-built musical instruments, such that a
program that reads data from the instrument (e.g. from an
[Arduino](http://www.arduino.cc/) USB serial port) can be connected to
the inputs of a sound synthesizer.  The first version of this software
was written entirely in Cycling 74's
[Max/MSP](http://www.cycling74.com/), but in order to make it more
universally useful and cross-platform, we decided to re-implement the
protocol in C; hence, libmapper.

We were already using [Open Sound Control](http://opensoundcontrol.org/)
for this purpose, but needed a way to dynamically change the
mappings--not only to modify the destinations of OSC messages, but
also to change scaling, or to introduce derivatives, integration,
frequency filtering, smoothing, etc.  Eventually this grew into the
use of a multicast bus to publish signal metadata and administrate
peer-to-peer connections between machines on a local subnet, with the
ability to specify arbitrary mathematical expressions that can be used
to perform signal conditioning.

It has been designed with the idea of decentralization in mind.  We
specifically wanted to avoid keeping a "master" node which is
responsible for arbitrating connections or republishing data, and we
also wanted to avoid needing to keep a central database of serial
numbers or other unique identifiers for devices produced in the lab.
Instead, common communication is performed on a multicast UDP port,
that all nodes listen on, and this is used to implement a
collision-handling protocol that can assign a unique ID to each device
that appears.  Actual signal data is sent directly from a sender
device to the receiver's IP and UDP port.

Advantages of libmapper
-----------------------

This library is, on one level, our response to the widely acknowledged
lack of a "query" protocol for Open Sound Control.  We are aware for
example of [current work on using the ZeroConf protocol for publishing
OSC services](http://sourceforge.net/projects/osctools/), or various
efforts to standardize a certain set of OSC address conventions.

However, we believe libmapper covers considerably more ground than
simply the ability to publish namespaces, and handles a direct need
that we experienced in our daily work.  We did evaluate the use of
ZeroConf early on in this work, but we eventually decided that sending
OSC messages over multicast was just as easy and required the
integration of fewer dependencies.  With the addition of being able to
control message routing and to dynamically specify signal conditioning
equations, we feel this work represents a significant enough effort
that it is worth making available to the general public.

It can also be seen as an open source alternative to some commercial
products such as Camille Troillard's [Osculator](http://www.osculator.net/)
and STEIM's [Junxion](http://www.steim.org/steim/junxion_v4.html),
albeit certainly more "barebones" for the moment.

A major difference in libmapper's approach to handling devices is the
idea that each "driver" can be a separate process, on the same or
different machines in the network.  By creating a C library with a
basic interface and by providing SWIG bindings, we hope to enable a
very wide variety of programming language bindings, to allow any
choice of development strategy.^[At this time, the SWIG bindings only
work for Python.  Support for more languages, particularly Java, is in
the works.]  Another advantage of a C library is portability: we have
had libmapper working on a Gumstix device, an ethernet-enabled
ARM-based microcomputer running Linux that can be easily embedded
directly into an instrument.

We also provide an external to integrate libmapper support into
Max/MSP or [PureData](http://puredata.info) patches.

Known conceptual limitations
----------------------------

The "devices and signals" metaphor encapsulated by libmapper is of
course not the be-all and end-all of mapping.  We assume homogeneous
vectors of floating-point or integer numbers, while in reality more
complex structured data might be useful to transmit.

Particularly if libmapper is to be used for visual art, transmission
of matrixes or even image formats might be interesting, although there
is no standard way to represent this in Open Sound Control other than
as a binary "blob".  It's possible that in this case a protocol such
as HTTP that supports MIME type headers is simply a more appropriate
choice.

That aside, mapping techniques based on table-lookup or other
data-oriented processes don't fit directly into the "connection"
paradigm used here.  Of course, many of these techniques can easily be
implemented as "intermediate" devices that sit between a sender and
receiver.  Another category of mappings that can currently only be
solved this way include many-to-one mappings--libmapper currently has
no special handling of multiple devices sending to one receiver, and
the values are simply interleaved, whereas what is intended is
probably some combining function such as addition, multiplication, or
thresholding.  It may however be possible to solve this latter issue
by using some form of receiver-side routers which can handle
expressions containing multiple input variables.  As an explanation
for why we have not yet handled the case of many-to-one mapping, we
feel that in practise it is actually quite rare to have a case where
such a combining function should be arbitrarily modifiable by a user.
In the real world, dependencies between signals often have a semantic
significance which would be better handled internally to the receiver
rather than in the mapper layer.  Nonetheless, we hope to tackle this
problem eventually.

One impact of peer-to-peer messaging is that it may suffer from
redundancy.  In general it may be more efficient to send all data once
to a central node, and then out once more to nodes that request it, at
the expense of weighing down the bandwidth of that particular central
node.  In libmapper's case, if 50 nodes subscribe to a particular
signal, it will be repeated that many times by the place where it
originated.  Dealing with centralized-vs-decentralized efficiency
issues by automatically optimizing decisions on how messages are
distributed and where routing takes place is not impossible, but
represents non-trivial work--for example, in libmapper the concept of
a "router" is actually an independent node that a device sends
messages to, which then translates and rebroadcasts; it just happens
that the router is embedded in the sender because that was the most
efficient place to have it in our scenario.  In any case, given that
optimal message passing efficiency is not a near-term goal, it goes
without saying that your choice of using libmapper should depend on
what type of network you envision: if you need a large number of
receivers for the same signal you might consider one of the many
alternatives, such as the [SenseWorld
DataNetwork](http://sensestage.hexagram.ca), a
[SuperCollider](http://supercollider.sourceforge.net/)-based mapping
framework based on the central-server approach.

Future plans
------------

We chose to use Open Sound Control for this because of its
compatibility with a wide variety of audio and media software.  It is
a practical way to transmit arbitrary data in a well-defined binary
format.  In practice, since the actual connections are peer-to-peer,
they could technically be implemented using a variety of protocols.
High-frequency data for example might be better transmitted using the
[JACK Audio Connection Kit](http://jackaudio.org) or something
similar.  In addition, on embedded devices, it might make sense to
"transmit" data between sensors and synthesizers through shared
memory, while still allowing the use of the libmapper admin protocol
and GUIs to dynamically experiment with mapping.

Although we provide a basic cross-platform GUI using
[Qt](http://qt.nokia.com/), the most advanced user interface is still
the Max/MSP implementation.  Although this works extremely well, it is
limited to platforms supported by Max/MSP.  (In other words, not
Linux.)  There currently are plans to create a web-based front-end
that will be cross-platform.  Better ways of visualizing and
interacting with the network are also an active research topic.

For implementing intermediate devices that "learn" mappings
automatically, a useful feature would be to request snapshots of the
states of all inputs on a destination device.  This has already been
implemented in the previous Max/MSP version, and we have examples of
neural network and SVM intermediates, but this feature has not yet
made it into the C version of libmapper.

Currently the only supported data types are 32-bit integers and 32-bit
floating point.  This covers most of our use cases, but we hope to
expand this catalogue eventually.

In the syntax for mathematical expressions, we include a method for
indexing previous values to allow basic filter construction.  This is
done by index, but we also implemented a syntax for accessing previous
state according to time in seconds.  This feature is not yet useable,
but in the future interpolation will be performed to allow referencing
of time-accurate values.

Currently any "filter" implemented by an expression may suffer
somewhat from jitter due to irregular timing in the sender, network
delay, etc.  We plan to tackle this problem by allowing timestamped
signals using OSC bundles, which will likely require some changes to
liblo, the OSC implementation used by libmapper.

License
-------

This software was written in the Input Devices and Music Interaction
Laboratory at McGill University in Montreal, and is copyright those
found in the AUTHORS file.  It is licensed under the GNU Lesser Public
General License version 2.1 or later.  Please see COPYING for details.

In accordance with the LGPL, you are allowed to use it in commercial
products provided it remains dynamically linked, such that libmapper
always remains free to modify.  If you'd like to use it in an
outstanding context, please contact the AUTHORS to seek an agreement.

Dependencies of libmapper are:

* [liblo](http://liblo.sourceforge.net), LGPL

Optional dependencies for the Python bindings:

* [SWIG](http://www.swig.org), GPL3, LGPL-compatible for generated code

* [Python](http://www.python.org), Python license, LGPL-compatible

Used only in the examples:

* [RtAudio](http://www.music.mcgill.ca/~gary/rtaudio), MIT license,
  LGPL-compatible