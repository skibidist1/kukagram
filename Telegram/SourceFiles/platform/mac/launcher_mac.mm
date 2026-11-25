/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "platform/mac/launcher_mac.h"

#include "core/crash_reports.h"
#include "core/update_checker.h"
#include "base/base_file_utilities.h"
#include "base/platform/base_platform_file_utilities.h"
#include "base/platform/mac/base_utilities_mac.h"

#include <Cocoa/Cocoa.h>
#include <CoreFoundation/CFURL.h>
#include <sys/sysctl.h>

namespace Platform {

Launcher::Launcher(int argc, char *argv[])
: Core::Launcher(argc, argv) {
}

void Launcher::initHook() {
	base::RegisterBundledResources(u"Telegram.rcc"_q);
}

bool Launcher::launchUpdater(UpdaterLaunch action) {
	if (cExeName().isEmpty()) {
		return false;
	}
	@autoreleasepool {

#ifdef TDESKTOP_DISABLE_AUTOUPDATE
	if (action == UpdaterLaunch::JustRelaunch) {
		@try {
			const auto bundle = [[NSBundle mainBundle] bundlePath];
			if (!bundle.length) {
				LOG(("Could not get the app path for relaunch."));
				return false;
			}
			// Arguments are passed as argv, never interpolated into shell code.
			// The helper outlives this process and waits for its complete exit.
			const auto script = @"pid=$1; app=$2; shift 2; attempts=0; "
				"while /bin/kill -0 "$pid" 2>/dev/null; do "
				"if [ "$attempts" -ge 600 ]; then exit 75; fi; "
				"/bin/sleep 0.1; attempts=$((attempts + 1)); done; "
				"exec /usr/bin/open -n -a "$app" --args "$@"";
			const auto arguments = [NSMutableArray arrayWithObjects:
				@"-c",
				script,
				@"kukagram-relaunch",
				[NSString stringWithFormat:@"%d",
					[[NSProcessInfo processInfo] processIdentifier]],
				bundle,
				@"-noupdate",
				@"-workdir",
				Q2NSString(cWorkingDir()),
				nil];
			if (cRestartingToSettings()) [arguments addObject:@"-tosettings"];
			if (cLaunchMode() == LaunchModeAutoStart) {
				[arguments addObject:@"-autostart"];
			}
			if (Logs::DebugEnabled()) [arguments addObject:@"-debug"];
			if (cStartInTray()) [arguments addObject:@"-startintray"];
			if (cDataFile() != u"data"_q) {
				[arguments addObject:@"-key"];
				[arguments addObject:Q2NSString(cDataFile())];
			}
			if (!cStartUrls().isEmpty()) {
				[arguments addObject:@"--"];
				for (const auto &url : cStartUrls()) {
					[arguments addObject:Q2NSString(url.toString(QUrl::FullyEncoded))];
				}
			}
			const auto helper = [[NSTask alloc] init];
			[helper setExecutableURL:[NSURL fileURLWithPath:@"/bin/sh"]];
			[helper setArguments:arguments];
			[helper setStandardInput:[NSFileHandle fileHandleWithNullDevice]];
			[helper setStandardOutput:[NSFileHandle fileHandleWithNullDevice]];
			[helper setStandardError:[NSFileHandle fileHandleWithNullDevice]];
			NSError *error = nil;
			if (![helper launchAndReturnError:&error]) {
				LOG(("Could not start the relaunch helper: %1").arg(
					error ? NS2QString([error localizedDescription]) : QString()));
				return false;
			}
			Logs::closeMain();
			CrashReports::Finish();
			return true;
		} @catch (NSException *exception) {
			LOG(("Could not start the relaunch helper."));
			return false;
		}
	}
#endif // TDESKTOP_DISABLE_AUTOUPDATE

#ifdef OS_MAC_STORE
	// In AppStore version we don't have Updater.
	// We just relaunch our app.
	if (action == UpdaterLaunch::JustRelaunch) {
		NSDictionary *conf = [NSDictionary dictionaryWithObject:[NSArray array] forKey:NSWorkspaceLaunchConfigurationArguments];
		[[NSWorkspace sharedWorkspace] launchApplicationAtURL:[NSURL fileURLWithPath:Q2NSString(cExeDir() + cExeName())] options:NSWorkspaceLaunchAsync | NSWorkspaceLaunchNewInstance configuration:conf error:0];
		return true;
	}
#endif // OS_MAC_STORE

	NSString *path = @"", *args = @"";
	@try {
		path = [[NSBundle mainBundle] bundlePath];
		if (!path) {
			LOG(("Could not get bundle path!!"));
			return false;
		}
		path = [path stringByAppendingString:@"/Contents/Frameworks/Updater"];
		base::Platform::RemoveQuarantine(QFile::decodeName([path fileSystemRepresentation]));

		NSMutableArray *args = [[NSMutableArray alloc] initWithObjects:@"-workpath", Q2NSString(cWorkingDir()), @"-procid", nil];
		[args addObject:[NSString stringWithFormat:@"%d", [[NSProcessInfo processInfo] processIdentifier]]];
		if (cRestartingToSettings()) [args addObject:@"-tosettings"];
		if (action == UpdaterLaunch::JustRelaunch) [args addObject:@"-noupdate"];
		if (cLaunchMode() == LaunchModeAutoStart) [args addObject:@"-autostart"];
		if (Logs::DebugEnabled()) [args addObject:@"-debug"];
		if (cStartInTray()) [args addObject:@"-startintray"];
		if (cDataFile() != u"data"_q) {
			[args addObject:@"-key"];
			[args addObject:Q2NSString(cDataFile())];
		}
		if (customWorkingDir()) {
			[args addObject:@"-workdir_custom"];
		}

		DEBUG_LOG(("Application Info: executing %1 %2").arg(NS2QString(path)).arg(NS2QString([args componentsJoinedByString:@" "])));
		Logs::closeMain();
		CrashReports::Finish();
		if (![NSTask launchedTaskWithLaunchPath:path arguments:args]) {
			DEBUG_LOG(("Task not launched while executing %1 %2").arg(NS2QString(path)).arg(NS2QString([args componentsJoinedByString:@" "])));
			return false;
		}
	}
	@catch (NSException *exception) {
		LOG(("Exception caught while executing %1 %2").arg(NS2QString(path)).arg(NS2QString(args)));
		return false;
	}
	@finally {
	}

	}
	return true;
}

} // namespace Platform
