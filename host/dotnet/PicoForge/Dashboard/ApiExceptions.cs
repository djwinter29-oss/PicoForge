namespace PicoForge;

internal sealed class ApiInputException(string message) : Exception(message);

internal class BoardStateException(string message) : InvalidOperationException(message);

internal sealed class BoardNotFoundException(string message) : Exception(message);
