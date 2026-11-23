/* The frame cap on iOS from the phone's state (tests/xivboot_ios.c, every 2 s on the main thread):
 * 30 a second while it is hot (thermal state serious or critical, until it is back to fair or
 * nominal), in Low Power Mode, or on battery; else 60. XIV_FPSCAP in run.args fixes the cap instead
 * (xivboot_ios.c does not call this then); XIV_BATTERY_FPS and XIV_HOT_FPS choose the lower caps
 * (0: no lower cap for that). */
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <stdlib.h>

int ios_power_fps(const char** why)
{
    static int hot, monitoring;
    @autoreleasepool
    {
        if (!monitoring)
        {
            [UIDevice currentDevice].batteryMonitoringEnabled = YES;
            monitoring = 1;
        }
        NSProcessInfoThermalState t = [NSProcessInfo processInfo].thermalState;
        if (t >= NSProcessInfoThermalStateSerious)
            hot = 1;
        else if (t <= NSProcessInfoThermalStateFair)
            hot = 0;
        const char* hot_env = getenv("XIV_HOT_FPS");
        const char* bat_env = getenv("XIV_BATTERY_FPS");
        int hot_fps = hot_env ? atoi(hot_env) : 30, bat_fps = bat_env ? atoi(bat_env) : 30;
        if (hot && hot_fps > 0)
        {
            *why = t >= NSProcessInfoThermalStateCritical ? "the phone is very hot" : "the phone is hot";
            return hot_fps;
        }
        if ([NSProcessInfo processInfo].lowPowerModeEnabled && bat_fps > 0)
        {
            *why = "Low Power Mode";
            return bat_fps;
        }
        UIDeviceBatteryState b = [UIDevice currentDevice].batteryState;
        if (b == UIDeviceBatteryStateUnplugged && bat_fps > 0)
        {
            *why = "on battery";
            return bat_fps;
        }
        *why = b == UIDeviceBatteryStateUnknown ? "battery state unknown" : "on power";
        return 60;
    }
}
