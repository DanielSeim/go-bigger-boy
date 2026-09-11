package com.danielseim.gbb;

/** Decides whether a library selection can return to the existing game. */
final class RomLaunchPolicy {
    private RomLaunchPolicy() {}

    static boolean shouldResumeExistingRom(boolean returnToGame,
                                           String selectedPath,
                                           String runningPath) {
        return returnToGame && selectedPath != null && runningPath != null &&
                !runningPath.isEmpty() && selectedPath.equals(runningPath);
    }
}
