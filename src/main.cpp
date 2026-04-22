#include "pch.h"

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
    F4SE::Init(a_f4se);
    REX::INFO("UnreadNotes loaded — CommonLibF4 bootstrap stub");
    return true;
}
