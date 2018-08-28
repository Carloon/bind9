/*
 * Copyright (C) Internet Systems Consortium, Inc. ("ISC")
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * See the COPYRIGHT file distributed with this work for additional
 * information regarding copyright ownership.
 */

#pragma once

/*! \file isc/string.h */

#ifdef _GNU_SOURCE
#undef _GNU_SOURCE
#define _DEFAULT_SOURCE 1
#include <string.h>
#define _GNU_SOURCE 1
#else /* _GNU_SOURCE */
#include <string.h>
#endif /* _GNU_SOURCE */

#include <isc/platform.h>
#include <isc/lang.h>

ISC_LANG_BEGINDECLS

size_t
isc_string_strlcpy(char *dst, const char *src, size_t size);

#ifdef ISC_PLATFORM_NEEDSTRLCPY
#define strlcpy isc_string_strlcpy
#endif

size_t
isc_string_strlcat(char *dst, const char *src, size_t size);

#ifdef ISC_PLATFORM_NEEDSTRLCAT
#define strlcat isc_string_strlcat
#endif

ISC_LANG_ENDDECLS
