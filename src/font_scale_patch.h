#pragma once
#include <windows.h>
namespace stardom {
bool InstallFractionalFontScalePatch(HMODULE executable);
class FittedLabelFontScope {
public:
    explicit FittedLabelFontScope(int pixels);
    ~FittedLabelFontScope();
    FittedLabelFontScope(const FittedLabelFontScope&) = delete;
    FittedLabelFontScope& operator=(const FittedLabelFontScope&) = delete;
private:
    int saved_;
};
}
