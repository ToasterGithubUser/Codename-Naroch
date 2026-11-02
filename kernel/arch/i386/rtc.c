#include <kernel/io.h>
#include <kernel/rtc.h>

#define CMOS_INDEX 0x70
#define CMOS_DATA 0x71
#define CMOS_SECONDS 0x00
#define CMOS_MINUTES 0x02
#define CMOS_HOURS 0x04
#define CMOS_DAY 0x07
#define CMOS_MONTH 0x08
#define CMOS_YEAR 0x09
#define CMOS_STATUS_A 0x0A
#define CMOS_STATUS_B 0x0B
#define CMOS_CENTURY 0x32
#define CMOS_UPDATE_IN_PROGRESS 0x80
#define RTC_READ_ATTEMPTS 100000U

typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint8_t year;
    uint8_t century;
    uint8_t status_b;
} rtc_snapshot_t;

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_INDEX, reg & 0x7F);
    return inb(CMOS_DATA);
}

static int cmos_read_snapshot(rtc_snapshot_t* snapshot) {
    for (uint32_t attempt = 0; attempt < RTC_READ_ATTEMPTS; attempt++) {
        if (cmos_read(CMOS_STATUS_A) & CMOS_UPDATE_IN_PROGRESS) {
            continue;
        }

        rtc_snapshot_t first = {
            .second = cmos_read(CMOS_SECONDS),
            .minute = cmos_read(CMOS_MINUTES),
            .hour = cmos_read(CMOS_HOURS),
            .day = cmos_read(CMOS_DAY),
            .month = cmos_read(CMOS_MONTH),
            .year = cmos_read(CMOS_YEAR),
            .century = cmos_read(CMOS_CENTURY),
            .status_b = cmos_read(CMOS_STATUS_B)
        };
        if (cmos_read(CMOS_STATUS_A) & CMOS_UPDATE_IN_PROGRESS) {
            continue;
        }

        rtc_snapshot_t second = {
            .second = cmos_read(CMOS_SECONDS),
            .minute = cmos_read(CMOS_MINUTES),
            .hour = cmos_read(CMOS_HOURS),
            .day = cmos_read(CMOS_DAY),
            .month = cmos_read(CMOS_MONTH),
            .year = cmos_read(CMOS_YEAR),
            .century = cmos_read(CMOS_CENTURY),
            .status_b = cmos_read(CMOS_STATUS_B)
        };
        if (first.second == second.second &&
            first.minute == second.minute &&
            first.hour == second.hour &&
            first.day == second.day &&
            first.month == second.month &&
            first.year == second.year &&
            first.century == second.century &&
            first.status_b == second.status_b) {
            *snapshot = second;
            return 1;
        }
    }
    return 0;
}

static uint8_t from_bcd(uint8_t value) {
    return (uint8_t)((value & 0x0F) + ((value >> 4) * 10));
}

int rtc_read_time(rtc_time_t* time) {
    if (!time) {
        return 0;
    }

    rtc_snapshot_t snapshot;
    if (!cmos_read_snapshot(&snapshot)) {
        return 0;
    }

    uint8_t hour = snapshot.hour;
    int is_pm = !(snapshot.status_b & 0x02) && (hour & 0x80);
    hour &= 0x7F;

    if (!(snapshot.status_b & 0x04)) {
        snapshot.second = from_bcd(snapshot.second);
        snapshot.minute = from_bcd(snapshot.minute);
        hour = from_bcd(hour);
        snapshot.day = from_bcd(snapshot.day);
        snapshot.month = from_bcd(snapshot.month);
        snapshot.year = from_bcd(snapshot.year);
        snapshot.century = from_bcd(snapshot.century);
    }

    if (!(snapshot.status_b & 0x02)) {
        hour = (uint8_t)(hour % 12);
        if (is_pm) {
            hour = (uint8_t)(hour + 12);
        }
    }

    uint16_t year = snapshot.century >= 19 && snapshot.century <= 99
        ? (uint16_t)snapshot.century * 100 + snapshot.year
        : (uint16_t)2000 + snapshot.year;

    if (snapshot.month < 1 || snapshot.month > 12 ||
        snapshot.day < 1 || snapshot.day > 31 ||
        hour > 23 || snapshot.minute > 59 || snapshot.second > 59) {
        return 0;
    }

    time->year = year;
    time->month = snapshot.month;
    time->day = snapshot.day;
    time->hour = hour;
    time->minute = snapshot.minute;
    time->second = snapshot.second;
    return 1;
}
