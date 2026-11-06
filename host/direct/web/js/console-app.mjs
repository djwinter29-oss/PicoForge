// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

/** @file Composition root for console.html: the only module of the console page that reads browser globals. */
import { mountConsole } from "./ui/console.mjs";

mountConsole({ document, window, location, fetch: (...args) => fetch(...args) });
