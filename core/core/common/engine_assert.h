#pragma once
#ifndef _DEBUG
#define ASSERT_MSG(cond, msg)
#define ASSERT_FAIL(msg)
#else
#include <cassert>
#define ASSERT_MSG(cond, msg, ...) assert((msg, cond))
#define ASSERT_FAIL(msg, ...) ASSERT_MSG(false, msg)
#endif