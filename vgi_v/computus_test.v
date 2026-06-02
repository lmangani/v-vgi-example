module vgi_v

fn test_easter_2025() {
	d := easter_sunday(2025) or { panic(err.msg()) }
	assert d.year == 2025
	assert d.month == 4
	assert d.day == 20
}

fn test_easter_batch() {
	years := [2024, 2025, 2026]
	months := [3, 4, 4]
	days := [31, 20, 5]
	for i, y in years {
		d := easter_sunday(y) or { panic(err.msg()) }
		assert d.month == months[i]
		assert d.day == days[i]
	}
}

fn test_date32_days_monotonic() {
	d1 := easter_sunday(2024)!
	d2 := easter_sunday(2025)!
	assert date32_days(d2) > date32_days(d1)
}

// Golden date32 values for C++ smoke_client (must match date32_days / Arrow date32).
fn test_date32_smoke_expected() {
	assert date32_days(easter_sunday(2024)!) == 19813
	assert date32_days(easter_sunday(2025)!) == 20198
	assert date32_days(easter_sunday(2026)!) == 20548
}
