// SPDX-License-Identifier: GPL-3.0-or-later
package com.darkmatter.openq4;

import java.io.File;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.Locale;

/**
 * A cheap identity for the installed archives. A complete check reads every
 * byte; once one has passed, an unchanged name/size/date listing lets later
 * launches start the game without repeating it.
 */
final class VerifiedData {
    private VerifiedData() {}

    /** Empty when the directory is unreadable or holds no archives. */
    static String fingerprint(File directory) {
        File[] children = directory == null ? null : directory.listFiles();
        if (children == null) return "";
        Arrays.sort(children);
        StringBuilder listing = new StringBuilder();
        for (File child : children) {
            if (!child.isFile() || !child.getName().toLowerCase(Locale.ROOT).endsWith(".pk4")) continue;
            listing.append(child.getName()).append(':').append(child.length()).append(':').append(child.lastModified()).append('\n');
        }
        if (listing.length() == 0) return "";
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(listing.toString().getBytes("UTF-8"));
            StringBuilder hex = new StringBuilder();
            for (byte b : digest) hex.append(String.format(Locale.ROOT, "%02x", b));
            return hex.toString();
        } catch (Exception unavailable) {
            return "";
        }
    }
}
