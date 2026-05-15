// This file Copyright © 2022-2023 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <type_traits>

#include <fmt/format.h>

#include "transmission.h"

#include "session-settings.h"
#include "variant.h"

void tr_session_settings::load(tr_variant* src)
{
#define V(key, field, type, default_value, comment) \
    if (auto* const child = tr_variantDictFind(src, key); child != nullptr) \
    { \
        if constexpr (std::is_same_v<type, tr_bandwidth_allocator_mode>) \
        { \
            if (auto val = libtransmission::VariantConverter::load<type>(child); val) \
            { \
                this->field = *val; \
            } \
            else \
            { \
                tr_logAddWarn("Invalid 'bandwidth_allocator' setting; using 'default'"); \
                this->field = tr_bandwidth_allocator_mode::Default; \
            } \
        } \
        else if (auto val = libtransmission::VariantConverter::load<type>(child); val) \
        { \
            this->field = *val; \
        } \
    }
    SESSION_SETTINGS_FIELDS(V)
#undef V
}

void tr_session_settings::save(tr_variant* tgt) const
{
#define V(key, field, type, default_value, comment) \
    tr_variantDictRemove(tgt, key); \
    libtransmission::VariantConverter::save<decltype(field)>(tr_variantDictAdd(tgt, key), field);
    SESSION_SETTINGS_FIELDS(V)
#undef V
}
