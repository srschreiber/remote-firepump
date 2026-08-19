# run_tests.ps1 — build and run the host unit test suite (Windows).
#
#   powershell -ExecutionPolicy Bypass -File src\arduino\tests\run_tests.ps1
#
# Set $env:CXX to point at a g++/clang++ if it is not already on PATH.

$ErrorActionPreference = 'Stop'

$here    = Split-Path -Parent $MyInvocation.MyCommand.Path
$fw      = Join-Path (Split-Path -Parent $here) 'fire_pump_controller'
$build   = Join-Path $here 'build'

$cxx = $env:CXX
if (-not $cxx) {
    $found = Get-Command g++ -ErrorAction SilentlyContinue
    if ($found) { $cxx = $found.Source }
}
if (-not $cxx) {
    $fallback = Join-Path $env:USERPROFILE 'tools\w64devkit\bin\g++.exe'
    if (Test-Path $fallback) { $cxx = $fallback }
}
if (-not $cxx) {
    Write-Error "No C++ compiler found. Install g++/MinGW or set `$env:CXX."
}

New-Item -ItemType Directory -Force $build | Out-Null

$sources = @(
    (Join-Path $here 'shim\Arduino.cpp'),
    (Join-Path $here 'test_main.cpp'),
    (Join-Path $here 'test_support.cpp'),
    (Join-Path $here 'test_pump_controller.cpp'),
    (Join-Path $here 'test_http_protocol.cpp'),
    (Join-Path $here 'test_api_handler.cpp'),
    (Join-Path $here 'test_invariants.cpp'),
    (Join-Path $here 'test_extensions.cpp'),
    (Join-Path $here 'test_valve_water.cpp'),
    (Join-Path $here 'test_event_log.cpp'),
    (Join-Path $fw   'pump_controller.cpp'),
    (Join-Path $fw   'http_protocol.cpp'),
    (Join-Path $fw   'api_handler.cpp'),
    (Join-Path $fw   'event_log.cpp')
)

$common = @(
    '-std=c++17', '-O1', '-g',
    '-Wall', '-Wextra', '-Wshadow', '-Wconversion', '-Wsign-conversion',
    '-Wold-style-cast', '-Wdouble-promotion', '-Wformat=2',
    # NOTE: -Wuseless-cast is deliberately NOT enabled. The rollover-safe
    # idiom `static_cast<uint32_t>(now - startedAt) >= duration` is required
    # by the design and documents intent; GCC flags the cast as redundant.
    '-Werror',
    "-I$here\shim",
    '-DPUMP_CONTROLLER_TEST_ACCESS'
)

$failed = $false

# The suite is built and run across the full configuration matrix:
#   * both relay polarities, so RELAY_ACTIVE_LOW is proven to be the single
#     point of control rather than assumed to be;
#   * maintenance API both enabled and disabled, so the flag's effect on
#     endpoint reachability is proven in each direction.
foreach ($polarity in @('true', 'false')) {
    foreach ($maint in @('1', '0')) {
        $pName = if ($polarity -eq 'true') { 'active-low' } else { 'active-high' }
        $mName = if ($maint -eq '1') { 'maint-on' } else { 'maint-off' }
        $name  = "$pName-$mName"
        $exe   = Join-Path $build "tests_$name.exe"

        Write-Host ""
        Write-Host "=== building host tests ($name) ===" -ForegroundColor Cyan
        & $cxx @common "-DRELAY_ACTIVE_LOW_OVERRIDE=$polarity" `
                       "-DENABLE_MAINTENANCE_API=$maint" @sources -o $exe
        if ($LASTEXITCODE -ne 0) { Write-Error "compilation failed ($name)" }

        Write-Host "=== running host tests ($name) ===" -ForegroundColor Cyan
        & $exe @args
        if ($LASTEXITCODE -ne 0) { $failed = $true }
    }
}

Write-Host ""
if ($failed) {
    Write-Host "HOST TEST SUITE FAILED" -ForegroundColor Red
    exit 1
}
Write-Host "HOST TEST SUITE PASSED (both polarities x maintenance on/off)" -ForegroundColor Green
exit 0
