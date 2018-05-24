/*
 * Copyright (c) 2017, Citrix Systems, Inc.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the names of its 
 *     contributors may be used to endorse or promote products derived from 
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "wrappers.h"

static int tests_running = 1;

FILE *
__wrap_fopen(void)
{
	return (FILE*) mock();
}

void __real_fclose(FILE *fp);

void
__wrap_fclose(FILE *fp)
{
	if (tests_running) {
		check_expected_ptr(fp);
	}
	__real_fclose(fp);
}

int
wrap_vprintf(const char *format, va_list ap)
{
	int bufsize = mock();
	char* buf = mock();

	int len = vsnprintf(buf, bufsize, format, ap);

	assert_in_range(len, 0, bufsize);

	return len;
}

int
__wrap_printf(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);

	return wrap_vprintf(format, ap);
}

int
__wrap___printf_chk (int __flag, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);

	return wrap_vprintf(format, ap);
}

char *setup_vprintf_mock(int size)
{
	char *buf;

	buf = malloc(size);

	will_return(wrap_vprintf, size);
	will_return(wrap_vprintf, buf);

	return buf;
}

void disable_mocks()
{
	tests_running = 0;
}