#pragma once
#include "gui_object.h"

namespace stardom {
// Map/TalkForm.txt Memo3. Use native text metrics rather than screen position:
// the dialogue controller moves its panel between scenes.
inline bool IsDialogueMemo(void* object) {
    if (!CanReadGuiObject(object) ||
        !CanReadGuiObject(static_cast<unsigned char*>(object) + 0x60)) return false;
    return GuiField<int>(object, GuiObjectField::width) == 406 &&
        GuiField<int>(object, GuiObjectField::height) == 112 &&
        GuiField<int>(object, 0x13C) == 20 &&
        // The controller changes line gap from authored 5 to live 8.
        // Gap is a presentation preference, not part of this identity.
        GuiField<int>(object, 0x11C) == 0 &&
        GuiField<int>(object, 0x120) == 2 &&
        GuiField<int>(object, 0x124) == 405 &&
        GuiField<int>(object, 0x128) == 110;
}
}
