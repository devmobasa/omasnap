#pragma once

#include <QString>

/** Deterministic responsiveness, cancellation, and lifetime checks for the
 * serialized scroll-capture coordinator. */
bool runScrollCaptureJobChecks(QString &error);
