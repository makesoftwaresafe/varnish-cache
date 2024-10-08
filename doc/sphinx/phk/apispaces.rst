..
	Copyright (c) 2017 Varnish Software AS
	SPDX-License-Identifier: BSD-2-Clause
	See LICENSE file for full text of license

.. _phk_api_spaces:

API spaces
==========

The reason you cant remember hearing about "API spaces" at university
is that I just made that concept up, as a title for this piece.

We need a name for the collision where APIs meet namespaces.

At some point in their career, most C programmers learn that ``j0``,
``j1``, ``y0`` and ``y1`` are names to avoid whereas ``j2`` and
``y2`` up to, but not including ``jn`` and ``yn`` are OK.

The reason is that somebody back when I was a child thought it would
be really neat if the math library supported Bessel functions,
without thinking about ``<math.h>`` as an API which had to coexist
in the flat namespace of the C language along many other APIs.

One of the big attractions of Object Oriented programming is that
it solves exactly that problem:
Nobody is confused about ``car->push()`` and ``stack->push()``.

But Varnish is written in C which has a flat namespace and we must
live with it.

From the very start, we defined cadastral boundaries in the flat
namespace by assigning VTLA prefixes to various chunks of code.

``VSB_something`` has to do with the sbufs we adopted from FreeBSD,
``VGC_something`` is Vcc Generated C-source and so on.

Mostly we have stuck with the 'V' prefix, which for some reason
is almost unused everywhere else, but we also have prominent
exceptions.  ``WS_something`` for workspaces for instance.

As long as all the C-code was in-project, inconsistencies and
the precise location of function prototypes didn't matter much,
it was "just something you had to know".

Now that we have VMODs, and even more so, now that we want to provide
some semblance of API stability for VMODs, we have a lot of sorting
and some renaming to do, in order to clearly delineate APIs within
our flat namespace and our include files.

Frederick P. Brooks points out in his classic "The Mythical Man-Month",
that is the difference between a program-product and a programming-product,
and he makes the case that the effort required triples, going from
the former to the latter.

Having spent some weeks on what I thought would be a three day task
I suspect that his was an underestimate.

I will now try to lay out what I think will be our policy on APIs
and name-space sharing going forward, but please understand that
this is mostly just an aspirational goal at this point.

General namespace rules
-----------------------

1. Each API or otherwise isolated section of code gets a unique
   prefix, ending with an underscore, (``VSB_``, ``V1F_`` etc.)

2. Public symbols has upper case prefix.

3. Private symbols use prefix in lower case, both as ``static``
   symbols in source files, and when exposed to other source
   files in the same section of code.

4. Friends-With-Benefit symbols have an additional underscore
   after the prefix:  ``FOO__bar()`` and are only to be used with
   explicit permission, which should be clearly documented in
   the relevant #include file.

VMOD API/ABI levels
-------------------

Vmods can be written against one of three API/ABI levels, called
respectively ``VRT``, ``PACKAGE`` and ``SOURCE``, defined in
detail below.

A VMOD which restricts itself to the ``VRT`` API/ABI gets maximum
stability and will, we hope, work without recompilation across
many major and minor releases of Varnish.

A VMOD which uses the ``PACKAGE`` API, will likely keep working
across minor releases of varnish releases, but will usually
need to be recompiled for new major releases of varnish.

A VMOD which uses the ``SOURCE`` API is compiled against one
specific version of Varnish, and will not work with another
version until recompiled.

The VMOD VRT API/ABI
--------------------

This API space could also have been called 'inline', because it
is basically what you see in the C-source generated by VCC:

| Include files allowed:
|
| 	``#include "vdef.h"``
| 	``#include "vrt.h"``
| 	``#include "vrt_obj.h"``
| 	``#include "vcl.h"``

Any private and Friends-With-Benefits symbols are off-limits
to VMODs, (it is usually stuff VCC needs for the compiled
code, and likely as not, you will crash if you mess with it.)

The ``"vrt.h"`` contains two #defines which together defines
the level of this API:

| 	``#define VRT_MAJOR_VERSION       6U``
| 	``#define VRT_MINOR_VERSION       2U``

A snapshot of these will be automatically compiled into the
VMOD shared library, and they will be checked for compatibility
when the VMOD is imported by the VCL compiler.

The VMOD PACKAGE API/ABI
------------------------

This API space provides access to everything in the ``VRT`` API
space plus the other exposed and supported APIs in varnishd.

| Include files allowed:
|
|	``#include "cache.h"		// NB: includes vdef.h and vrt.h``
|	``#include "cache_backend.h"``
|	``#include "cache_director.h"``
|	``#include "cache_filter.h"``
|	``#include "waiter/waiter.h"``

Any private and Friends-With-Benefits symbols are off-limits
to VMODs.

In addition to the two-part VRT version, ``"cache.h"`` will
contain two #defines for levels of this API.

| 	``#define PACKAGE_MAJOR_VERSION       1U``
| 	``#define PACKAGE MINOR_VERSION       3U``

Compile-time snapshots of these will be checked, along with
their VRT cousins be checked for compatibility on VMOD import.

The VMOD SOURCE API/ABI
-----------------------

This API space provides access to private parts of varnishd and its
use is highly discouraged, unless you absolutely have to,

You can #include any file from the varnish source tree and use
anything you find in them - but don't come crying to us if it
all ends in tears:  No refunds at this window.

A hash value of all the .h files in the source tree will be
compiled into the VMOD and will be checked to match exactly
on VMOD import.

*phk*
