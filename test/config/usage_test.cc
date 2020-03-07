//

#include <abel/config/flags/internal/usage.h>

#include <sstream>

#include <gtest/gtest.h>
#include <abel/config/flags/flag.h>
#include <abel/config/flags/internal/path_util.h>
#include <abel/config/flags/internal/program_name.h>
#include <abel/config/flags/parse.h>
#include <abel/config/flags/usage.h>
#include <abel/config/flags/usage_config.h>
#include <abel/memory/memory.h>
#include <abel/strings/starts_with.h>
#include <abel/strings/contain.h>

ABEL_FLAG(int, usage_reporting_test_flag_01, 101,
          "usage_reporting_test_flag_01 help message");
ABEL_FLAG(bool, usage_reporting_test_flag_02, false,
          "usage_reporting_test_flag_02 help message");
ABEL_FLAG(double, usage_reporting_test_flag_03, 1.03,
          "usage_reporting_test_flag_03 help message");
ABEL_FLAG(int64_t, usage_reporting_test_flag_04, 1000000000000004L,
          "usage_reporting_test_flag_04 help message");

static const char kTestUsageMessage[] = "Custom usage message";

struct UDT {
    UDT() = default;

    UDT(const UDT &) = default;
};

bool abel_parse_flag(abel::string_view, UDT *, std::string *) { return true; }

std::string abel_unparse_flag(const UDT &) { return "UDT{}"; }

ABEL_FLAG(UDT, usage_reporting_test_flag_05, {},
          "usage_reporting_test_flag_05 help message");

ABEL_FLAG(
        std::string, usage_reporting_test_flag_06, {},
        "usage_reporting_test_flag_06 help message.\n"
        "\n"
        "Some more help.\n"
        "Even more long long long long long long long long long long long long "
        "help message.");

namespace {

    namespace flags = abel::flags_internal;

    static std::string NormalizeFileName(abel::string_view fname) {
#ifdef _WIN32
        std::string normalized(fname);
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        fname = normalized;
#endif

        auto abel_pos = fname.rfind("abel/");
        if (abel_pos != abel::string_view::npos) {
            fname = fname.substr(abel_pos);
        }
        return std::string(fname);
    }

    class UsageReportingTest : public testing::Test {
    protected:
        UsageReportingTest() {
            // Install default config for the use on this unit test.
            // Binary may install a custom config before tests are run.
            abel::FlagsUsageConfig default_config;
            default_config.normalize_filename = &NormalizeFileName;
            abel::SetFlagsUsageConfig(default_config);
        }

    private:
        flags::FlagSaver flag_saver_;
    };

// --------------------------------------------------------------------

    using UsageReportingDeathTest = UsageReportingTest;

    TEST_F(UsageReportingDeathTest, TestSetProgramUsageMessage) {
        EXPECT_EQ(abel::ProgramUsageMessage(), kTestUsageMessage);

#ifndef _WIN32
        // TODO(rogeeff): figure out why this does not work on Windows.
        EXPECT_DEATH(abel::SetProgramUsageMessage("custom usage message"),
                     ".*SetProgramUsageMessage\\(\\) called twice.*");
#endif
    }

// --------------------------------------------------------------------

    TEST_F(UsageReportingTest, TestFlagHelpHRF_on_flag_01) {
        const auto *flag = flags::FindCommandLineFlag("usage_reporting_test_flag_01");
        std::stringstream test_buf;

        flags::FlagHelp(test_buf, *flag, flags::HelpFormat::kHumanReadable);
        EXPECT_EQ(
                test_buf.str(),
                R"(    --usage_reporting_test_flag_01 (usage_reporting_test_flag_01 help message);
      default: 101;
)");
    }

    TEST_F(UsageReportingTest, TestFlagHelpHRF_on_flag_02) {
        const auto *flag = flags::FindCommandLineFlag("usage_reporting_test_flag_02");
        std::stringstream test_buf;

        flags::FlagHelp(test_buf, *flag, flags::HelpFormat::kHumanReadable);
        EXPECT_EQ(
                test_buf.str(),
                R"(    --usage_reporting_test_flag_02 (usage_reporting_test_flag_02 help message);
      default: false;
)");
    }

    TEST_F(UsageReportingTest, TestFlagHelpHRF_on_flag_03) {
        const auto *flag = flags::FindCommandLineFlag("usage_reporting_test_flag_03");
        std::stringstream test_buf;

        flags::FlagHelp(test_buf, *flag, flags::HelpFormat::kHumanReadable);
        EXPECT_EQ(
                test_buf.str(),
                R"(    --usage_reporting_test_flag_03 (usage_reporting_test_flag_03 help message);
      default: 1.03;
)");
    }

    TEST_F(UsageReportingTest, TestFlagHelpHRF_on_flag_04) {
        const auto *flag = flags::FindCommandLineFlag("usage_reporting_test_flag_04");
        std::stringstream test_buf;

        flags::FlagHelp(test_buf, *flag, flags::HelpFormat::kHumanReadable);
        EXPECT_EQ(
                test_buf.str(),
                R"(    --usage_reporting_test_flag_04 (usage_reporting_test_flag_04 help message);
      default: 1000000000000004;
)");
    }

    TEST_F(UsageReportingTest, TestFlagHelpHRF_on_flag_05) {
        const auto *flag = flags::FindCommandLineFlag("usage_reporting_test_flag_05");
        std::stringstream test_buf;

        flags::FlagHelp(test_buf, *flag, flags::HelpFormat::kHumanReadable);
        EXPECT_EQ(
                test_buf.str(),
                R"(    --usage_reporting_test_flag_05 (usage_reporting_test_flag_05 help message);
      default: UDT{};
)");
    }

// --------------------------------------------------------------------

    TEST_F(UsageReportingTest, TestFlagsHelpHRF) {
        std::string usage_test_flags_out =
                R"(usage_test: Custom usage message

  Flags from abel/test/config/usage_test.cc:
    --usage_reporting_test_flag_01 (usage_reporting_test_flag_01 help message);
      default: 101;
    --usage_reporting_test_flag_02 (usage_reporting_test_flag_02 help message);
      default: false;
    --usage_reporting_test_flag_03 (usage_reporting_test_flag_03 help message);
      default: 1.03;
    --usage_reporting_test_flag_04 (usage_reporting_test_flag_04 help message);
      default: 1000000000000004;
    --usage_reporting_test_flag_05 (usage_reporting_test_flag_05 help message);
      default: UDT{};
    --usage_reporting_test_flag_06 (usage_reporting_test_flag_06 help message.

      Some more help.
      Even more long long long long long long long long long long long long help
      message.); default: "";
)";

        std::stringstream test_buf_01;
        flags::FlagsHelp(test_buf_01, "usage_test.cc",
                         flags::HelpFormat::kHumanReadable, kTestUsageMessage);
        EXPECT_EQ(test_buf_01.str(), usage_test_flags_out);

        std::stringstream test_buf_02;
        flags::FlagsHelp(test_buf_02, "config/usage_test.cc",
                         flags::HelpFormat::kHumanReadable, kTestUsageMessage);
        EXPECT_EQ(test_buf_02.str(), usage_test_flags_out);

        std::stringstream test_buf_03;
        flags::FlagsHelp(test_buf_03, "usage_test", flags::HelpFormat::kHumanReadable,
                         kTestUsageMessage);
        EXPECT_EQ(test_buf_03.str(), usage_test_flags_out);

        std::stringstream test_buf_04;
        flags::FlagsHelp(test_buf_04, "config/invalid_file_name.cc",
                         flags::HelpFormat::kHumanReadable, kTestUsageMessage);
        EXPECT_EQ(test_buf_04.str(),
                  R"(usage_test: Custom usage message

  No modules matched: use -helpfull
)");

        std::stringstream test_buf_05;
        flags::FlagsHelp(test_buf_05, "", flags::HelpFormat::kHumanReadable,
                         kTestUsageMessage);
        std::string test_out = test_buf_05.str();
        abel::string_view test_out_str(test_out);
        EXPECT_TRUE(
                abel::starts_with(test_out_str, "usage_test: Custom usage message"));
        EXPECT_TRUE(abel::string_contains(
                test_out_str, "Flags from abel/test/config/usage_test.cc:"));
        EXPECT_TRUE(abel::string_contains(test_out_str,
                                          "Flags from abel/test/config/usage_test.cc:"));
        EXPECT_TRUE(
                abel::string_contains(test_out_str, "-usage_reporting_test_flag_01 "));
        EXPECT_TRUE(abel::string_contains(test_out_str, "-help (show help"))
                            << test_out_str;
    }

// --------------------------------------------------------------------

    TEST_F(UsageReportingTest, TestNoUsageFlags) {
        std::stringstream test_buf;
        EXPECT_EQ(flags::HandleUsageFlags(test_buf, kTestUsageMessage), -1);
    }

// --------------------------------------------------------------------

    TEST_F(UsageReportingTest, TestUsageFlag_helpshort) {
        abel::SetFlag(&FLAGS_helpshort, true);

        std::stringstream test_buf;
        EXPECT_EQ(flags::HandleUsageFlags(test_buf, kTestUsageMessage), 1);
        EXPECT_EQ(test_buf.str(),
                  R"(usage_test: Custom usage message

  Flags from abel/test/config/usage_test.cc:
    --usage_reporting_test_flag_01 (usage_reporting_test_flag_01 help message);
      default: 101;
    --usage_reporting_test_flag_02 (usage_reporting_test_flag_02 help message);
      default: false;
    --usage_reporting_test_flag_03 (usage_reporting_test_flag_03 help message);
      default: 1.03;
    --usage_reporting_test_flag_04 (usage_reporting_test_flag_04 help message);
      default: 1000000000000004;
    --usage_reporting_test_flag_05 (usage_reporting_test_flag_05 help message);
      default: UDT{};
    --usage_reporting_test_flag_06 (usage_reporting_test_flag_06 help message.

      Some more help.
      Even more long long long long long long long long long long long long help
      message.); default: "";
)");
    }

// --------------------------------------------------------------------

    TEST_F(UsageReportingTest, TestUsageFlag_help) {
        abel::SetFlag(&FLAGS_help, true);

        std::stringstream test_buf;
        EXPECT_EQ(flags::HandleUsageFlags(test_buf, kTestUsageMessage), 1);
        EXPECT_EQ(test_buf.str(),
                  R"(usage_test: Custom usage message

  Flags from abel/test/config/usage_test.cc:
    --usage_reporting_test_flag_01 (usage_reporting_test_flag_01 help message);
      default: 101;
    --usage_reporting_test_flag_02 (usage_reporting_test_flag_02 help message);
      default: false;
    --usage_reporting_test_flag_03 (usage_reporting_test_flag_03 help message);
      default: 1.03;
    --usage_reporting_test_flag_04 (usage_reporting_test_flag_04 help message);
      default: 1000000000000004;
    --usage_reporting_test_flag_05 (usage_reporting_test_flag_05 help message);
      default: UDT{};
    --usage_reporting_test_flag_06 (usage_reporting_test_flag_06 help message.

      Some more help.
      Even more long long long long long long long long long long long long help
      message.); default: "";

Try --helpfull to get a list of all flags.
)");
    }

// --------------------------------------------------------------------

    TEST_F(UsageReportingTest, TestUsageFlag_helppackage) {
        abel::SetFlag(&FLAGS_helppackage, true);

        std::stringstream test_buf;
        EXPECT_EQ(flags::HandleUsageFlags(test_buf, kTestUsageMessage), 1);
        EXPECT_EQ(test_buf.str(),
                  R"(usage_test: Custom usage message

  Flags from abel/test/config/usage_test.cc:
    --usage_reporting_test_flag_01 (usage_reporting_test_flag_01 help message);
      default: 101;
    --usage_reporting_test_flag_02 (usage_reporting_test_flag_02 help message);
      default: false;
    --usage_reporting_test_flag_03 (usage_reporting_test_flag_03 help message);
      default: 1.03;
    --usage_reporting_test_flag_04 (usage_reporting_test_flag_04 help message);
      default: 1000000000000004;
    --usage_reporting_test_flag_05 (usage_reporting_test_flag_05 help message);
      default: UDT{};
    --usage_reporting_test_flag_06 (usage_reporting_test_flag_06 help message.

      Some more help.
      Even more long long long long long long long long long long long long help
      message.); default: "";

Try --helpfull to get a list of all flags.
)");
    }

// --------------------------------------------------------------------

    TEST_F(UsageReportingTest, TestUsageFlag_version) {
        abel::SetFlag(&FLAGS_version, true);

        std::stringstream test_buf;
        EXPECT_EQ(flags::HandleUsageFlags(test_buf, kTestUsageMessage), 0);
#ifndef NDEBUG
        EXPECT_EQ(test_buf.str(), "usage_test\nDebug build (NDEBUG not #defined)\n");
#else
        EXPECT_EQ(test_buf.str(), "usage_test\n");
#endif
    }

// --------------------------------------------------------------------

    TEST_F(UsageReportingTest, TestUsageFlag_only_check_args) {
        abel::SetFlag(&FLAGS_only_check_args, true);

        std::stringstream test_buf;
        EXPECT_EQ(flags::HandleUsageFlags(test_buf, kTestUsageMessage), 0);
        EXPECT_EQ(test_buf.str(), "");
    }

// --------------------------------------------------------------------

    TEST_F(UsageReportingTest, TestUsageFlag_helpon) {
        abel::SetFlag(&FLAGS_helpon, "bla-bla");

        std::stringstream test_buf_01;
        EXPECT_EQ(flags::HandleUsageFlags(test_buf_01, kTestUsageMessage), 1);
        EXPECT_EQ(test_buf_01.str(),
                  R"(usage_test: Custom usage message

  No modules matched: use -helpfull
)");

        abel::SetFlag(&FLAGS_helpon, "usage_test");

        std::stringstream test_buf_02;
        EXPECT_EQ(flags::HandleUsageFlags(test_buf_02, kTestUsageMessage), 1);
        EXPECT_EQ(test_buf_02.str(),
                  R"(usage_test: Custom usage message

  Flags from abel/test/config/usage_test.cc:
    --usage_reporting_test_flag_01 (usage_reporting_test_flag_01 help message);
      default: 101;
    --usage_reporting_test_flag_02 (usage_reporting_test_flag_02 help message);
      default: false;
    --usage_reporting_test_flag_03 (usage_reporting_test_flag_03 help message);
      default: 1.03;
    --usage_reporting_test_flag_04 (usage_reporting_test_flag_04 help message);
      default: 1000000000000004;
    --usage_reporting_test_flag_05 (usage_reporting_test_flag_05 help message);
      default: UDT{};
    --usage_reporting_test_flag_06 (usage_reporting_test_flag_06 help message.

      Some more help.
      Even more long long long long long long long long long long long long help
      message.); default: "";
)");
    }

// --------------------------------------------------------------------

}  // namespace

int main(int argc, char *argv[]) {
    (void) abel::GetFlag(FLAGS_undefok);  // Force linking of parse.cc
    flags::SetProgramInvocationName("usage_test");
    abel::SetProgramUsageMessage(kTestUsageMessage);
    ::testing::InitGoogleTest(&argc, argv);

    return RUN_ALL_TESTS();
}
