#include "app/control_request.h"
#include "test_support.h"

#include <initializer_list>

using audiolens::app::ControlRequest;
using audiolens::app::parseControlRequest;

namespace {

ControlRequest parse(std::initializer_list<const char*> arguments) {
    QStringList list;
    for (const char* argument : arguments) {
        list << QString::fromLatin1(argument);
    }
    return parseControlRequest(list);
}

}  // namespace

AL_TEST(control_request_reads_each_kind_of_value) {
    const ControlRequest request =
        parse({"--preset", "movie", "--volume", "70", "--balance", "-5", "--bass", "20",
               "--clarity", "80", "--leveling", "35", "--bypass", "on", "--on"});

    CHECK(request.error.isEmpty());
    CHECK(request.preset.has_value() && *request.preset == QStringLiteral("movie"));
    CHECK_EQ(request.volume.value_or(-1), 70);
    CHECK_EQ(request.balance.value_or(999), -5);
    CHECK_EQ(request.bass.value_or(-1), 20);
    CHECK_EQ(request.clarity.value_or(-1), 80);
    CHECK_EQ(request.leveling.value_or(-1), 35);
    CHECK(request.bypass.value_or(false));
    CHECK(request.power.value_or(false));
}

AL_TEST(control_request_leaves_unmentioned_settings_alone) {
    // The reason every field is optional. A command line is a set of
    // adjustments, and `--preset movie` must not also move the volume to some
    // default the user never asked for.
    const ControlRequest request = parse({"--preset", "movie"});

    CHECK(request.preset.has_value());
    CHECK(!request.volume.has_value());
    CHECK(!request.balance.has_value());
    CHECK(!request.bass.has_value());
    CHECK(!request.power.has_value());
    CHECK(!request.bypass.has_value());
}

AL_TEST(control_request_rejects_a_value_out_of_range) {
    // Rejected, not clamped. `--volume 700` is a typo, and quietly setting the
    // volume to 100 would hide it.
    CHECK(!parse({"--volume", "101"}).error.isEmpty());
    CHECK(!parse({"--volume", "-1"}).error.isEmpty());
    CHECK(!parse({"--balance", "51"}).error.isEmpty());
    CHECK(!parse({"--balance", "-51"}).error.isEmpty());
    CHECK(!parse({"--bass", "101"}).error.isEmpty());

    CHECK(parse({"--volume", "0"}).error.isEmpty());
    CHECK(parse({"--volume", "100"}).error.isEmpty());
    CHECK(parse({"--balance", "-50"}).error.isEmpty());
    CHECK(parse({"--balance", "50"}).error.isEmpty());
}

AL_TEST(control_request_rejects_a_value_that_is_not_a_number) {
    CHECK(!parse({"--volume", "loud"}).error.isEmpty());
    // A flag where a value should be. Without this the next flag would be eaten
    // as the value, and `--volume --status` would report nothing while looking
    // like it had worked.
    CHECK(!parse({"--volume", "--status"}).error.isEmpty());
    CHECK(!parse({"--volume"}).error.isEmpty());
    CHECK(!parse({"--preset"}).error.isEmpty());
}

AL_TEST(control_request_rejects_an_unknown_flag) {
    // An unknown flag is nearly always a misspelt known one. Carrying on would
    // apply every other flag on the line while dropping the one that was meant.
    const ControlRequest request = parse({"--preset", "movie", "--volumme", "70"});
    CHECK(!request.error.isEmpty());
    CHECK(request.error.contains(QStringLiteral("--volumme")));
}

AL_TEST(control_request_reads_bypass_only_as_on_or_off) {
    CHECK(parse({"--bypass", "on"}).bypass.value_or(false));
    CHECK(!parse({"--bypass", "off"}).bypass.value_or(true));
    CHECK(parse({"--bypass", "ON"}).bypass.value_or(false));  // case does not matter
    CHECK(!parse({"--bypass", "1"}).error.isEmpty());
    CHECK(!parse({"--bypass"}).error.isEmpty());
}

AL_TEST(control_request_keeps_volume_step_separate_from_volume) {
    // Balance runs from -50 to +50, so a leading minus already means "an
    // absolute position on the left". If it meant "relative" on the volume and
    // "absolute" on the balance, every hotkey anyone wrote would be a coin toss.
    const ControlRequest down = parse({"--volume-step", "-5"});
    CHECK(down.error.isEmpty());
    CHECK_EQ(down.volumeStep.value_or(0), -5);
    CHECK(!down.volume.has_value());

    const ControlRequest absolute = parse({"--volume", "5"});
    CHECK_EQ(absolute.volume.value_or(0), 5);
    CHECK(!absolute.volumeStep.has_value());
}

AL_TEST(control_request_knows_what_a_running_instance_can_act_on) {
    // A bare launch, and one carrying only the startup-time options, have
    // nothing to say to a process that is already up. Both mean "show me the
    // window", not "do nothing".
    CHECK(!parse({}).actsOnRunningInstance());
    CHECK(!parse({"--minimized"}).actsOnRunningInstance());
    CHECK(!parse({"--verbose"}).actsOnRunningInstance());

    CHECK(parse({"--status"}).actsOnRunningInstance());
    CHECK(parse({"--preset", "movie"}).actsOnRunningInstance());
    CHECK(parse({"--toggle"}).actsOnRunningInstance());
    CHECK(parse({"--quit"}).actsOnRunningInstance());
}

AL_TEST(control_request_separates_asking_from_changing) {
    // What decides whether a command line is worth starting the app for when
    // none is running. Starting one to report that it is running answers
    // itself, and leaves behind the thing that was being asked about.
    CHECK(!parse({"--status"}).changesState());
    CHECK(!parse({"--list-presets"}).changesState());
    CHECK(!parse({"--status", "--list-presets"}).changesState());

    CHECK(parse({"--preset", "movie"}).changesState());
    CHECK(parse({"--volume", "50"}).changesState());
    CHECK(parse({"--off"}).changesState());
    CHECK(parse({"--bypass", "on"}).changesState());
    CHECK(parse({"--show"}).changesState());
    CHECK(parse({"--quit"}).changesState());

    // Mixed: the question rides along with something that does change, so the
    // whole line is worth acting on.
    CHECK(parse({"--preset", "movie", "--status"}).changesState());
}

AL_TEST(control_request_stops_at_the_first_bad_flag) {
    // Reported once. A line with two mistakes is still one mistake to fix
    // first, and the second message is usually a consequence of the first.
    const ControlRequest request = parse({"--nope", "--also-nope"});
    CHECK(request.error.contains(QStringLiteral("--nope")));
    CHECK(!request.error.contains(QStringLiteral("--also-nope")));
}
