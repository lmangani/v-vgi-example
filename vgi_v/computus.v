module vgi_v

import time

// Western (Gregorian) Easter Sunday — Anonymous Gregorian algorithm.
pub fn easter_sunday(year int) !time.Time {
	if year < 1583 {
		return error('year must be >= 1583 (Gregorian calendar)')
	}
	a := year % 19
	b, c := year / 100, year % 100
	d, e := b / 4, b % 4
	f := (b + 8) / 25
	g := (b - f + 1) / 3
	h := (19 * a + b - d - g + 15) % 30
	i, k := c / 4, c % 4
	ell := (32 + 2 * e + 2 * i - h - k) % 7
	m := (a + 11 * h + 22 * ell) / 451
	month := (h + ell - 7 * m + 114) / 31
	day := ((h + ell - 7 * m + 114) % 31) + 1
	return time.Time{year: year, month: month, day: day}
}

pub fn date32_days(t time.Time) i32 {
	return i32(t.unix() / 86400)
}
