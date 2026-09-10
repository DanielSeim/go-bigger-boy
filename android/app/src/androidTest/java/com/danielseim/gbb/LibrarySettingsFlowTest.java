package com.danielseim.gbb;

import static androidx.test.espresso.Espresso.onView;
import static androidx.test.espresso.action.ViewActions.click;
import static androidx.test.espresso.action.ViewActions.scrollTo;
import static androidx.test.espresso.assertion.ViewAssertions.matches;
import static androidx.test.espresso.matcher.ViewMatchers.isDisplayed;
import static androidx.test.espresso.matcher.ViewMatchers.withContentDescription;
import static androidx.test.espresso.matcher.ViewMatchers.withText;

import androidx.test.ext.junit.rules.ActivityScenarioRule;
import androidx.test.ext.junit.runners.AndroidJUnit4;

import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;

/** Exercises the user-visible library/settings navigation on a real device. */
@RunWith(AndroidJUnit4.class)
public final class LibrarySettingsFlowTest {
    @Rule
    public ActivityScenarioRule<LibraryActivity> activity =
            new ActivityScenarioRule<>(LibraryActivity.class);

    @Test
    public void librarySettingsAndAudioFlowIsReachable() {
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
        onView(withText("Recently played")).check(matches(isDisplayed()));
    }
}
