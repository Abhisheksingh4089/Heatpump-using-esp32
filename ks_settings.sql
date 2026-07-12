-- phpMyAdmin SQL Dump
-- version 5.2.1
-- https://www.phpmyadmin.net/
--
-- Host: localhost:3306
-- Generation Time: Jul 12, 2026
-- Server version: 5.7.23-23
-- PHP Version: 8.1.34
--
-- PURPOSE:
--   Remote settings table for ESP32 Heat Pump Controller.
--   The server writes a new row (status=0 → PENDING).
--   The ESP32 polls GET /iot_api/get_settings.php?device_id=... every 5 sec.
--   When a pending row is found the ESP32 applies it, saves to NVS, then
--   calls POST /iot_api/ack_settings.php to mark status=1 (APPLIED).
--
-- STATUS VALUES:
--   0 = PENDING  — written by server, not yet applied by device
--   1 = APPLIED  — device confirmed; settings are live in NVS
--   2 = FAILED   — device could not apply (feedback field has reason)
-- ============================================================

SET SQL_MODE = "NO_AUTO_VALUE_ON_ZERO";
START TRANSACTION;
SET time_zone = "+00:00";

/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;
/*!40101 SET @OLD_CHARACTER_SET_RESULTS=@@CHARACTER_SET_RESULTS */;
/*!40101 SET @OLD_COLLATION_CONNECTION=@@COLLATION_CONNECTION */;
/*!40101 SET NAMES utf8mb4 */;

-- ============================================================
-- Database: `saisiipd_crmdb_new`
-- ============================================================

-- --------------------------------------------------------
-- Table structure for `ks_settings`
-- --------------------------------------------------------

CREATE TABLE `ks_settings` (
  -- ── Identity ─────────────────────────────────────────────
  `id`                    int(11)       NOT NULL AUTO_INCREMENT,
  `company_id`            int(11)       NOT NULL DEFAULT 1,
  `device_id`             varchar(64)   NOT NULL COMMENT 'Matches ESP32 deviceId (MAC-based)',

  -- ── Temperature Control (maps to ConfigProfile) ──────────
  `set_temp`              decimal(6,2)  NOT NULL DEFAULT 40.00  COMMENT 'tempSetpoint °C — HP stops at this temp',
  `hysteresis`            decimal(5,2)  NOT NULL DEFAULT 2.00   COMMENT 'tempHysteresis °C — HP starts at (setpoint - hyst)',
  `heater_setpoint`       decimal(6,2)  NOT NULL DEFAULT 40.00  COMMENT 'heaterSetpoint °C — Heater stop temp',
  `heater_hysteresis`     decimal(5,2)  NOT NULL DEFAULT 2.00   COMMENT 'heaterHysteresis °C',

  -- ── Manual Override Flags ────────────────────────────────
  `heater_manually_on`    tinyint(1)    NOT NULL DEFAULT 0      COMMENT '1 = force heater ON, 0 = auto',
  `hp_manually_on`        tinyint(1)    NOT NULL DEFAULT 0      COMMENT '1 = force heat pump ON, 0 = auto',

  -- ── Control Sensor Selection ─────────────────────────────
  `control_sensor_idx`    tinyint(3)    NOT NULL DEFAULT 0      COMMENT 'DS18B20 array index (0-7) driving HP+Heater loop',

  -- ── PZEM Protection Thresholds ───────────────────────────
  `voltage_max`           decimal(6,2)  NOT NULL DEFAULT 253.00 COMMENT 'voltageMax V  (230V +10%)',
  `voltage_min`           decimal(6,2)  NOT NULL DEFAULT 196.00 COMMENT 'voltageMin V  (230V -15%)',
  `current_max`           decimal(6,2)  NOT NULL DEFAULT 20.00  COMMENT 'currentMax A',

  -- ── Temperature Alarm Limits ─────────────────────────────
  `temp_high_limit`       decimal(6,2)  NOT NULL DEFAULT 70.00  COMMENT 'tempHighLimit °C — alarm above this',
  `temp_low_limit`        decimal(6,2)  NOT NULL DEFAULT 0.00   COMMENT 'tempLowLimit  °C — alarm below this',

  -- ── Water Tank Parameters ────────────────────────────────
  `empty_dist_cm`         decimal(7,2)  NOT NULL DEFAULT 35.00  COMMENT 'emptyDistanceCm — sensor reading when tank EMPTY',
  `full_dist_cm`          decimal(7,2)  NOT NULL DEFAULT 5.00   COMMENT 'fullDistanceCm  — sensor reading when tank FULL',
  `water_shutoff_pct`     decimal(5,2)  NOT NULL DEFAULT 15.00  COMMENT 'waterShutoffPercent  — HP off below this %',
  `water_low_alarm_pct`   decimal(5,2)  NOT NULL DEFAULT 25.00  COMMENT 'waterLowAlarmPercent — alarm below this %',

  -- ── Sampling Intervals (milliseconds) ────────────────────
  `cloud_interval_ms`     int(11)       NOT NULL DEFAULT 5000   COMMENT 'cloudIntervalMs — telemetry push cadence',

  -- ── Handshake / Status ───────────────────────────────────
  -- 0=PENDING | 1=APPLIED | 2=FAILED
  `status`                tinyint(4)    NOT NULL DEFAULT 0      COMMENT '0=PENDING 1=APPLIED 2=FAILED',
  `feedback`              text                                   COMMENT 'ESP32 acknowledgement message or error reason',

  -- ── Timestamps ───────────────────────────────────────────
  `created_at`            datetime      NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at`            datetime      NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,

  PRIMARY KEY (`id`),
  KEY `idx_device_status` (`device_id`, `status`),      -- fast pending-settings lookup
  KEY `idx_device_latest` (`device_id`, `created_at`)   -- latest-row lookup

) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  COMMENT='Remote settings pushed from server to ESP32. Polled every 5 s by device.';

-- ============================================================
-- Sample seed row — matches ESP32 defaults
-- ============================================================
INSERT INTO `ks_settings` (
  `company_id`,
  `device_id`,
  `set_temp`,
  `hysteresis`,
  `heater_setpoint`,
  `heater_hysteresis`,
  `heater_manually_on`,
  `hp_manually_on`,
  `control_sensor_idx`,
  `voltage_max`,
  `voltage_min`,
  `current_max`,
  `temp_high_limit`,
  `temp_low_limit`,
  `empty_dist_cm`,
  `full_dist_cm`,
  `water_shutoff_pct`,
  `water_low_alarm_pct`,
  `cloud_interval_ms`,
  `status`,
  `feedback`,
  `created_at`,
  `updated_at`
  1,
  'HP-E1FC3F',   -- Real ESP32 device ID from serial monitor
  40.00,         -- tempSetpoint
  2.00,          -- tempHysteresis
  40.00,         -- heaterSetpoint
  2.00,          -- heaterHysteresis
  0,             -- heater_manually_on = false
  0,             -- hp_manually_on     = false
  0,             -- control_sensor_idx = Sensor[0]
  253.00,        -- voltage_max
  196.00,        -- voltage_min
  20.00,         -- current_max
  70.00,         -- temp_high_limit
  0.00,          -- temp_low_limit
  35.00,         -- empty_dist_cm
  5.00,          -- full_dist_cm
  15.00,         -- water_shutoff_pct
  25.00,         -- water_low_alarm_pct
  5000,          -- cloud_interval_ms
  0,             -- status = 0 (PENDING) so the ESP32 will immediately download and apply it!
  'Seed row — defaults applied.',
  NOW(),
  NOW()
);

COMMIT;

/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
