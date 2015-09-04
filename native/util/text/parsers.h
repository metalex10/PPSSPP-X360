#pragma once

#include <string>

#include "base/basictypes.h"

#undef major
#undef minor

// Parses version strings of the format "Major.Minor.Sub" and lets you interact with them conveniently.
struct Version {
	Version() : major(0), minor(0), sub(0) {}
	Version(const std::string &str) {
		if (!ParseVersionString(str)) {
			major = -1;
			minor = -1;
			sub = -1;
		}
	}

	int major;
	int minor;
	int sub;

	bool IsValid() const {
		return sub >= 0 && minor >= 0 && major >= 0;
	}

	bool operator == (const Version &other) const {
		return major == other.major && minor == other.minor && sub == other.sub;
	}
	bool operator != (const Version &other) const {
		return !(*this == other);
	}

	bool operator <(const Version &other) const {
		if (major < other.major) return true;
		if (major > other.major) return false;
		if (minor < other.minor) return true;
		if (minor > other.minor) return false;
		if (sub < other.sub) return true;
		if (sub > other.sub) return false;
		return false;
	}

	bool operator >=(const Version &other) const {
		return !(*this < other);
	}

	std::string ToString() const;
private:
	bool ParseVersionString(std::string str);
};

bool ParseMacAddress(std::string str, uint8_t macAddr[6]);