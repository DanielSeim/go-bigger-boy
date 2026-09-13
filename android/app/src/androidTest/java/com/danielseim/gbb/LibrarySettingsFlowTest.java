package com.danielseim.gbb;

import static androidx.test.espresso.Espresso.onView;
import static androidx.test.espresso.action.ViewActions.click;
import static androidx.test.espresso.action.ViewActions.scrollTo;
import static androidx.test.espresso.assertion.ViewAssertions.matches;
import static androidx.test.espresso.matcher.ViewMatchers.isDisplayed;
import static androidx.test.espresso.matcher.ViewMatchers.withContentDescription;
import static androidx.test.espresso.matcher.ViewMatchers.withText;
import static org.hamcrest.Matchers.anyOf;

import android.content.Intent;

import androidx.test.ext.junit.rules.ActivityScenarioRule;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;

/** Exercises the user-visible library/settings navigation on a real device. */
@RunWith(AndroidJUnit4.class)
public final class LibrarySettingsFlowTest {
    @Rule
    public ActivityScenarioRule<LibraryActivity> activity =
            new ActivityScenarioRule<>(new Intent(
                    InstrumentationRegistry.getInstrumentation().getTargetContext(),
                    LibraryActivity.class).putExtra(
                    LibraryActivity.EXTRA_SKIP_UPDATE_CHECK, true));

    @Test
    public void librarySettingsAndAudioFlowIsReachable() {
        // Android may restore the dashboard on its last selected tab. Start
        // from a known state so this test covers navigation, not restoration.
        onView(anyOf(withContentDescription("Library"),
                withContentDescription("Library, selected"))).perform(click());
        onView(withText("Recently played")).check(matches(isDisplayed()));
        onView(withContentDescription("Settings")).perform(click());
        onView(withText("Display")).check(matches(isDisplayed()));
        onView(withText(AudioSettingModel.LABEL)).check(matches(isDisplayed()));

        // Two clicks exercise the persisted setting without leaving the test
        // device with a modified preference, regardless of its initial value.
        onView(withText(AudioSettingModel.LABEL)).perform(click(), click());

        onView(withText("Remote link cable")).perform(scrollTo())
                .check(matches(isDisplayed()));
        onView(withContentDescription("Back to library")).perform(click());
        onView(withContentDescription("Library, selected"))
                .check(matches(isDisplayed()));
        onView(withText("Recently played")).check(matches(isDisplayed()));
    }
}
