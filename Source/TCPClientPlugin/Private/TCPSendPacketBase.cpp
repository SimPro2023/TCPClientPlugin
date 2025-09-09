#include "TCPSendPacketBase.h"

void UTCPSendPacketBase::ConvertToBytes(TCPBufferWriter& writer)
{
    BufferWriter = &writer;
    ConvertToBytesBP();
}

UTCPSendPacketBase* UTCPSendPacketBase::CreateSendPacketBP(TSubclassOf<UTCPSendPacketBase> packet)
{
    if (!*packet) return nullptr; // Prüfen, ob Subclass gültig

    // packet.Get() liefert den UClass*, korrekt für NewObject
    UTCPSendPacketBase* newObject = NewObject<UTCPSendPacketBase>(GetTransientPackage(), packet.Get());
    return newObject;
}
