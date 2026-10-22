// The provisioning console: what provision.ps1 sends, and what a typo or a
// hostile byte does to it.
#include <unity.h>

#include <cstring>

#include "NetSettings.h"

using namespace linknet;

void setUp(void) {}
void tearDown(void) {}

static void test_hex_decodes_any_printable_name(void) {
    char out[NetSettings::kSsidCap];
    // "Mon réseau 2,4" in UTF-8: a space, a comma and a two-byte letter.
    TEST_ASSERT_TRUE(decodeHexText("4d6f6e2072c3a97365617520322c34", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("Mon r\xc3\xa9seau 2,4", out);
    TEST_ASSERT_TRUE(decodeHexText("4D6F6E", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("Mon", out);
    TEST_ASSERT_TRUE(decodeHexText("", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_TRUE(decodeHexText("41\r", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("A", out);
}

static void test_hex_rejects_bad_input(void) {
    char out[4];
    TEST_ASSERT_FALSE(decodeHexText("414", out, sizeof out));       // odd
    TEST_ASSERT_FALSE(decodeHexText("4g", out, sizeof out));        // not hex
    TEST_ASSERT_FALSE(decodeHexText("410042", out, sizeof out));    // NUL inside
    TEST_ASSERT_TRUE(decodeHexText("414243", out, sizeof out));     // 3 + NUL fits
    TEST_ASSERT_FALSE(decodeHexText("41424344", out, sizeof out));  // 4 + NUL does not
}

static void test_a_full_session(void) {
    NetSettings s;
    TEST_ASSERT_FALSE(s.complete());
    TEST_ASSERT_EQUAL_INT((int)ProvisionAction::Updated, (int)provisionLine("ssid 4d6f6e", s).action);
    ProvisionReply r = provisionLine("pass 3132333435363738", s);
    TEST_ASSERT_EQUAL_INT((int)ProvisionAction::Updated, (int)r.action);
    TEST_ASSERT_EQUAL_STRING("OK pass set (8 chars)", r.message);
    TEST_ASSERT_EQUAL_INT((int)ProvisionAction::Error, (int)provisionLine("save", s).action);
    r = provisionLine("ota 6162636465666768696a6b6c", s);
    TEST_ASSERT_EQUAL_STRING("OK ota set (12 chars)", r.message);
    TEST_ASSERT_TRUE(s.complete());
    TEST_ASSERT_EQUAL_INT((int)ProvisionAction::Save, (int)provisionLine("save\r", s).action);
    TEST_ASSERT_EQUAL_INT((int)ProvisionAction::Test, (int)provisionLine("test", s).action);
    TEST_ASSERT_EQUAL_STRING("Mon", s.ssid);
    TEST_ASSERT_EQUAL_STRING("12345678", s.pass);
    TEST_ASSERT_EQUAL_STRING("abcdefghijkl", s.otaPass);

    char d[128];
    describeSettings(s, d, sizeof d);
    TEST_ASSERT_EQUAL_STRING("ssid=Mon pass=8 chars ota=12 chars", d);
}

static void test_passphrase_rules(void) {
    NetSettings s;
    // Open network.
    TEST_ASSERT_EQUAL_INT((int)ProvisionAction::Updated, (int)provisionLine("pass", s).action);
    TEST_ASSERT_EQUAL_STRING("", s.pass);
    TEST_ASSERT_EQUAL_INT((int)ProvisionAction::Updated, (int)provisionLine("pass ", s).action);
    // 7 characters: too short for WPA2, and the old value stays.
    provisionLine("pass 3132333435363738", s);
    TEST_ASSERT_EQUAL_INT((int)ProvisionAction::Error, (int)provisionLine("pass 31323334353637", s).action);
    TEST_ASSERT_EQUAL_STRING("12345678", s.pass);
    TEST_ASSERT_EQUAL_INT((int)ProvisionAction::Error, (int)provisionLine("pass zz", s).action);
}

static void test_values_are_never_echoed(void) {
    NetSettings s;
    const ProvisionReply r = provisionLine("pass 7365637265742d706173737068726173", s);
    TEST_ASSERT_NULL(strstr(r.message, "secret"));
    char d[128];
    describeSettings(s, d, sizeof d);
    TEST_ASSERT_NULL(strstr(d, "secret"));
}

static void test_rejected_lines_change_nothing(void) {
    NetSettings s;
    provisionLine("ssid 4d6f6e", s);
    TEST_ASSERT_EQUAL_INT((int)ProvisionAction::Error, (int)provisionLine("ssid", s).action);
    // 33 bytes: one more than 802.11 allows.
    TEST_ASSERT_EQUAL_INT((int)ProvisionAction::Error,
                          (int)provisionLine("ssid 414141414141414141414141414141414141414141414141414141414141414141", s).action);
    TEST_ASSERT_EQUAL_STRING("Mon", s.ssid);
    TEST_ASSERT_EQUAL_INT((int)ProvisionAction::Error, (int)provisionLine("ota 6162", s).action);
    TEST_ASSERT_EQUAL_INT((int)ProvisionAction::Error, (int)provisionLine("ssidd 41", s).action);
    TEST_ASSERT_EQUAL_INT((int)ProvisionAction::Error, (int)provisionLine("reboot", s).action);
    TEST_ASSERT_EQUAL_INT((int)ProvisionAction::None, (int)provisionLine("  \r", s).action);
    TEST_ASSERT_EQUAL_INT((int)ProvisionAction::Erase, (int)provisionLine("erase", s).action);
    TEST_ASSERT_EQUAL_STRING("", s.ssid);
    TEST_ASSERT_EQUAL_INT((int)ProvisionAction::Error, (int)provisionLine("test", s).action);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_hex_decodes_any_printable_name);
    RUN_TEST(test_hex_rejects_bad_input);
    RUN_TEST(test_a_full_session);
    RUN_TEST(test_passphrase_rules);
    RUN_TEST(test_values_are_never_echoed);
    RUN_TEST(test_rejected_lines_change_nothing);
    return UNITY_END();
}
