// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

namespace PicoForge;

internal sealed class ApiInputException(string message) : Exception(message);

internal class BoardStateException(string message) : InvalidOperationException(message);

internal sealed class BoardNotFoundException(string message) : Exception(message);
