#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <util/dstr.h>

static void dstr_basic_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct dstr s1 = {0};
	struct dstr s2 = {0};
	const struct dstr empty = {0};

	dstr_init_copy(&s2, "abc");
	dstr_ncopy_dstr(&s1, &s2, 2);
	assert_string_equal(s1.array, "ab");

	dstr_ncopy_dstr(&s1, &s2, 1);
	assert_string_equal(s1.array, "a");

	dstr_cat_dstr(&s1, &empty);
	assert_string_equal(s1.array, "a");

	dstr_ncat_dstr(&s1, &s2, 1);
	assert_string_equal(s1.array, "aa");

	dstr_insert(&s2, 1, "d");
	assert_string_equal(s2.array, "adbc");

	dstr_remove(&s2, 1, 1);
	assert_string_equal(s2.array, "abc");

	dstr_cat(&s2, "DEF");
	dstr_to_lower(&s2);
	assert_string_equal(s2.array, "abcdef");
}

static void dstr_mbs_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct dstr s1 = {0};

	dstr_from_mbs(&s1, "a");
	assert_string_equal(s1.array, "a");

	char *ret = dstr_to_mbs(&s1);
	assert_string_equal(ret, "a");
	bfree(ret);
}

static void str_util_test(void **state)
{
	UNUSED_PARAMETER(state);
	assert_int_equal(astrcmpi(NULL, NULL), 0);
	assert_int_equal(astrcmpi(NULL, "a"), -1);
	assert_int_equal(astrcmpi("a", NULL), 1);
	assert_int_equal(astrcmpi("a", "A"), 0);

	assert_int_equal(wstrcmpi(NULL, NULL), 0);
	assert_int_equal(wstrcmpi(NULL, L"a"), -1);
	assert_int_equal(wstrcmpi(L"a", NULL), 1);
	assert_int_equal(wstrcmpi(L"a", L"A"), 0);

	assert_int_equal(astrcmp_n(NULL, NULL, 0), 0);
	assert_int_equal(astrcmp_n(NULL, NULL, 1), 0);
	assert_int_equal(astrcmp_n(NULL, "a", 1), -1);
	assert_int_equal(astrcmp_n("a", NULL, 1), 1);
	assert_int_equal(astrcmp_n(NULL, "a", 2), -1);
	assert_int_equal(astrcmp_n("a", NULL, 2), 1);
	assert_int_equal(astrcmp_n("ab", "ac", 1), 0);

	assert_int_equal(wstrcmp_n(NULL, NULL, 0), 0);
	assert_int_equal(wstrcmp_n(NULL, NULL, 1), 0);
	assert_int_equal(wstrcmp_n(NULL, L"a", 1), -1);
	assert_int_equal(wstrcmp_n(L"a", NULL, 1), 1);
	assert_int_equal(wstrcmp_n(NULL, L"a", 2), -1);
	assert_int_equal(wstrcmp_n(L"a", NULL, 2), 1);
	assert_int_equal(wstrcmp_n(L"ab", L"ac", 1), 0);

	assert_int_equal(astrcmpi_n(NULL, NULL, 0), 0);
	assert_int_equal(astrcmpi_n(NULL, NULL, 1), 0);
	assert_int_equal(astrcmpi_n(NULL, "a", 1), -1);
	assert_int_equal(astrcmpi_n("a", NULL, 1), 1);
	assert_int_equal(astrcmpi_n(NULL, "a", 2), -1);
	assert_int_equal(astrcmpi_n("a", NULL, 2), 1);
	assert_int_equal(astrcmpi_n("Ab", "ac", 1), 0);

	assert_int_equal(wstrcmpi_n(NULL, NULL, 0), 0);
	assert_int_equal(wstrcmpi_n(NULL, NULL, 1), 0);
	assert_int_equal(wstrcmpi_n(NULL, L"a", 1), -1);
	assert_int_equal(wstrcmpi_n(L"a", NULL, 1), 1);
	assert_int_equal(wstrcmpi_n(NULL, L"a", 2), -1);
	assert_int_equal(wstrcmpi_n(L"a", NULL, 2), 1);
	assert_int_equal(wstrcmpi_n(L"Ab", L"ac", 1), 0);

	const char *text = "ababba";
	assert_ptr_equal(astrstri(text, "abb"), text + 2);
	assert_ptr_equal(astrstri(text, NULL), NULL);

	const wchar_t *wtext = L"ababba";
	assert_ptr_equal(wstrstri(wtext, L"abb"), wtext + 2);
	assert_ptr_equal(wstrstri(wtext, NULL), NULL);

	char text_depad[] = " ab ";
	assert_string_equal(strdepad(text_depad), "ab");
	assert_ptr_equal(strdepad(NULL), NULL);

	wchar_t wtext_depad[] = L" ab ";
	assert_string_equal(wcsdepad(wtext_depad), L"ab");
	assert_ptr_equal(wcsdepad(NULL), NULL);
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(dstr_basic_test),
		cmocka_unit_test(dstr_mbs_test),
		cmocka_unit_test(str_util_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
