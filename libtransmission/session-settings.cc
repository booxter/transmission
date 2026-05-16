// This file Copyright © 2022-2023 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <fmt/format.h>

#include "transmission.h"

#include "session-settings.h"
#include "variant.h"

namespace
{

template<typename T>
void load_session_setting(T& field, tr_variant* src)
{
    if (auto val = libtransmission::VariantConverter::load<T>(src); val)
    {
        field = *val;
    }
}

template<>
void load_session_setting<tr_bandwidth_allocator_mode>(tr_bandwidth_allocator_mode& field, tr_variant* src)
{
    if (auto val = libtransmission::VariantConverter::load<tr_bandwidth_allocator_mode>(src); val)
    {
        field = *val;
    }
    else
    {
        tr_logAddWarn("Invalid 'bandwidth_allocator' setting; using 'default'");
        field = tr_bandwidth_allocator_mode::Default;
    }
}

template<>
void load_session_setting<tr_strict_bandwidth_curve>(tr_strict_bandwidth_curve& field, tr_variant* src)
{
    if (auto val = libtransmission::VariantConverter::load<tr_strict_bandwidth_curve>(src); val)
    {
        field = *val;
    }
    else
    {
        tr_logAddWarn("Invalid 'bandwidth_strict_limited_curve' setting; using 'balanced'");
        field = tr_strict_bandwidth_curve::Balanced;
    }
}

} // namespace

void tr_session_settings::load(tr_variant* src)
{
#define V(key, field, type, default_value, comment) \
    if (auto* const child = tr_variantDictFind(src, key); child != nullptr) \
    { \
        load_session_setting(this->field, child); \
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
