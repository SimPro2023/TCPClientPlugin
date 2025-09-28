// Copyright 2022. Elogen Co. All Rights Reserved.

#include "TCPSessionBase.h"

#include "TCPClientController.h"
#include "TCPSendPacketBase.h"
#include "TCPRecvPacketBase.h"
#include "TCPHeaderComponent.h"

#include "Serialization/BufferArchive.h"
#include "TCPBufferReader.h"
#include "TCPBufferWriter.h"


bool UTCPSessionBase::IsConnected()
{
	return Controller != nullptr && Controller->IsConnected();
}

void UTCPSessionBase::SendPacket(ITCPSendPacket& sendPacket)
{
	if (Controller == nullptr)
	{
		UE_LOG(LogTemp, Error, TEXT("Send Fail. TCPClientController is null"));
		return;
	}
	if (!Controller->IsConnected())
	{
		UE_LOG(LogTemp, Error, TEXT("Send Fail. Session is Disconnected"));
		return;
	}
	if (!Header)
	{
		UE_LOG(LogTemp, Error, TEXT("Send Fail: Header is null"));
		return;
	}
	TSharedRef<TCPBufferWriter, ESPMode::ThreadSafe> ref(new TCPBufferWriter());
	TCPBufferWriter& writer = ref.Get();

	writer.Reserve(Header->GetHeaderSize());
	sendPacket.ConvertToBytes(writer);
	int id = sendPacket.GetPacketId();
	Header->WriteHeader(writer, id);

	FByteArrayRef SendBuffPtr = ref;
	Controller->StartSend(SendBuffPtr);
}

void UTCPSessionBase::SendPacketBP(UTCPSendPacketBase* sendPacket)
{
	if (sendPacket == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("Sendpacket is null"))
		check(sendPacket != nullptr)
		return;
	}
	SendPacket(*sendPacket);
}

void UTCPSessionBase::RegisterRecvPacket(TSubclassOf<UTCPRecvPacketBase> recvPacket)
{
	if (!recvPacket)
	{
		UE_LOG(LogTemp, Warning, TEXT("RegisterRecvPacket: recvPacket is null"));
		return;
	}
	auto c = *recvPacket;
	auto cdo = recvPacket.GetDefaultObject();
	if (!cdo)
	{
		UE_LOG(LogTemp, Warning, TEXT("RegisterRecvPacket: DefaultObject is null"));
		return;
	}
	RecvPacketMap.Add(cdo->GetPacketId(), *recvPacket);
}

void UTCPSessionBase::OnStart()
{
	if (CustomHeader)
	{
		Header = NewObject<UTCPHeaderComponent>(this, CustomHeader);
	}
	else
	{
		Header = NewObject<UTCPHeaderComponent>();
	}
	Controller->SetHeader(Header);

	for (auto recvPacket : PacketToReceive)
	{
		RegisterRecvPacket(recvPacket);
	}

	OnStartBP();
}

void UTCPSessionBase::OnRecv(int32 id, TCPBufferReader& reader)
{
	auto item = RecvPacketMap.Find(id);
	if (!item)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnRecv: No registered packet with id %d"), id);
		return;
	}
	if (item != nullptr)
	{
		auto packet = UTCPRecvPacketBase::CreateRecvPacketBP(*item);
		if (!packet)
		{
			UE_LOG(LogTemp, Warning, TEXT("OnRecv: Failed to create packet for id %d"), id);
			return;
		}
		packet->ConvertFromBytes(reader);
		OnRecvBP(packet);
	}
}

void UTCPSessionBase::OnSend(int32 id, int32 contentsByteSize, const TArray<uint8>& fullByteArray)
{
	OnSendBP(id, contentsByteSize, fullByteArray);
}

void UTCPSessionBase::OnDestroy()
{
	OnDestroyBP();
	RecvPacketMap.Empty();
}

void UTCPSessionBase::ConnectedCallback(bool success)
{
	OnConnectedBP(success);
	if (OnConnected.IsBound())
	{
		OnConnected.Execute(SessionName, success);
	}
}

void UTCPSessionBase::DisconnectedCallback(bool normalShutdown)
{
	OnDisconnectedBP(normalShutdown);
	if (OnDisconnected.IsBound())
	{
		OnDisconnected.Execute(SessionName, normalShutdown);
	}
}

void UTCPSessionBase::RecvMessageCallback(FByteArrayRef& messageByte)
{
	// ----- 1. Basic sanity checks -----
	if (!Header)
	{
		UE_LOG(LogTemp, Error, TEXT("RecvMessageCallback: Header is null"));
		return;
	}
	if (!Controller)
	{
		UE_LOG(LogTemp, Error, TEXT("RecvMessageCallback: Controller is null"));
		return;
	}
	if (!messageByte.IsValid() || messageByte->Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("RecvMessageCallback: Empty or invalid message buffer"));
		return;
	}

	uint8* buffer = messageByte->GetData();
	if (!buffer)
	{
		UE_LOG(LogTemp, Error, TEXT("RecvMessageCallback: Buffer pointer is null"));
		return;
	}

	// ----- 2. Size and integrity validation -----
	const int32 headerSize = Header->GetHeaderSize();
	const int32 totalSize = Header->ReadTotalSize(buffer);
	const int32 bufferSize = messageByte->Num();

	if (headerSize <= 0)
	{
		UE_LOG(LogTemp, Error, TEXT("RecvMessageCallback: Invalid header size %d"), headerSize);
		return;
	}

	if (totalSize < headerSize)
	{
		UE_LOG(LogTemp, Error,
			TEXT("RecvMessageCallback: Total size %d is smaller than header size %d"),
			totalSize, headerSize);
		return;
	}

	if (totalSize > bufferSize)
	{
		UE_LOG(LogTemp, Error,
			TEXT("RecvMessageCallback: Total size %d exceeds buffer size %d"),
			totalSize, bufferSize);
		return;
	}

	const int32 contentsSize = totalSize - headerSize;
	if (contentsSize < 0)
	{
		UE_LOG(LogTemp, Error,
			TEXT("RecvMessageCallback: Negative contents size %d"), contentsSize);
		return;
	}

	// Integrity check
	if (!Header->CheckIntegrity(buffer))
	{
		UE_LOG(LogTemp, Error,
			TEXT("RecvMessageCallback: Integrity check failed. Disconnecting client."));
		Controller->Disconnect(TEXT("Invalid data received. Integrity check unsuccessful."), false);
		return;
	}

	// ----- 3. Read protocol once -----
	const int32 protocolId = Header->ReadProtocol(buffer);

	// ----- 4. Create reader safely -----
	if (contentsSize == 0)
	{
		// Empty payload is valid – pass an empty reader
		TCPBufferReader reader;
		OnRecv(protocolId, reader);
	}
	else
	{
		const int32 dataOffset = headerSize;

		// Final range check before slicing the buffer
		if (dataOffset + contentsSize > bufferSize)
		{
			UE_LOG(LogTemp, Error,
				TEXT("RecvMessageCallback: Out-of-range read. Offset %d + size %d > buffer %d"),
				dataOffset, contentsSize, bufferSize);
			return;
		}

		TCPBufferReader reader(&buffer[dataOffset], contentsSize);
		OnRecv(protocolId, reader);
	}
}

void UTCPSessionBase::SendMessageCallback(FByteArrayRef& messageByte)
{
	if (!Header || !messageByte.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("SendMessageCallback: Header or message invalid"));
		return;
	}
	uint8* buffer = messageByte->GetData();
	if (!buffer)
	{
		UE_LOG(LogTemp, Warning, TEXT("SendMessageCallback: Buffer is null"));
		return;
	}
	int32 contentsSize = Header->ReadTotalSize(buffer) - Header->GetHeaderSize();
	OnSend(Header->ReadProtocol(buffer), contentsSize, *messageByte);
}

void UTCPSessionBase::SetController(TCPClientController* controller)
{
	Controller = controller;
}

TCPClientController* UTCPSessionBase::GetController()
{
	return Controller;
}
