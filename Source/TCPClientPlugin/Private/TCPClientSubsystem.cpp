// Copyright 2022. Elogen Co. All Rights Reserved.

#include "TCPClientSubsystem.h"
#include "TCPClientController.h"
#include "SocketSubsystem.h"
#include "Async/Async.h"
#include "Kismet/GameplayStatics.h"

void UTCPClientSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    
}

void UTCPClientSubsystem::Deinitialize()
{
    TMap<FString, UTCPSessionBase*> copyList = Sessions;
    for (auto& kvp : copyList)
    {
        DisconnectSessionByName(kvp.Key);
    }
}

UTCPSessionBase* UTCPClientSubsystem::ConnectSession(TSubclassOf<UTCPSessionBase> session)
{
    if (session == nullptr)
    {
        UE_LOG(LogTemp, Warning, TEXT("TCPClientSubSystem[StartSession Fail] : Input session is null.."));
    }

    UTCPSessionBase* newSession = NewObject<UTCPSessionBase>(this, session, TEXT("TCPSession"));
    DisconnectSessionByName(newSession->GetName());
    Sessions.Add(newSession->GetName(), newSession);

    TCPClientController* controller = new TCPClientController();
    controller->SetSession(newSession);
    newSession->SetController(controller);
    newSession->OnConnected.BindLambda(
    [this](const FString& SessionName, bool bSuccess)
        {
        AsyncTask(ENamedThreads::GameThread, [this, SessionName, bSuccess]()
           {
               if (IsValid(this))
               {
                   ConnectedCallback(SessionName, bSuccess);
               }
           });
       }
    );
    newSession->OnDisconnected.BindLambda(
    [this](const FString& SessionName, bool bNormalShutdown)
       {
           // Diese Lambda läuft evtl. auf einem Worker-Thread
            AsyncTask(ENamedThreads::GameThread,
               [this, SessionName, bNormalShutdown]()
                {
                if (IsValid(this))
                   {
                       DisConnectedCallback(SessionName, bNormalShutdown);
                   }
               }
            );
        }
    );


    newSession->OnStart();
    
    if (newSession->DNS)
    {
        GetDomainIpAddress(newSession->GetIp(),
            [this, newSession](FString ip, bool success)
            {
                if (success)
                    newSession->GetController()->StartConnect(ip, newSession->GetPort());
                else
                {
                    UE_LOG(LogTemp, Error, TEXT("Failed to resolve the DNS address"))
                    ConnectedCallback(newSession->GetName(), false);
                }
            }
        );
    }
    else
        newSession->GetController()->StartConnect(newSession->GetIp(), newSession->GetPort());
   
    return newSession;
}

UTCPSessionBase* UTCPClientSubsystem::ConnectSession(TSubclassOf<UTCPSessionBase> session, const FConnectedSessionDelegate& connectDelegate, const FDisconnectedSessionDelegate& disconnectDelegate)
{
    if (session == nullptr)
    {
        UE_LOG(LogTemp, Warning, TEXT("TCPClientSubSystem[StartSession Fail] : Input session is null.."));
    }

    const FString& sessionName = session.GetDefaultObject()->GetName();
    if (connectDelegate.IsBound())
    {
        OnConnected.Add(connectDelegate);
        RegisteredConnectDelegates.Add(sessionName, &connectDelegate);
    }
    if (disconnectDelegate.IsBound())
    {
        OnDisconnected.Add(disconnectDelegate);
        RegisteredDisconnectDelegates.Add(sessionName, &disconnectDelegate);
    }

    return ConnectSession(session);
}

void UTCPClientSubsystem::DisconnectSession(UTCPSessionBase* session)
{
    if (session != nullptr)
    {
        const FString& sessionName = session->GetName();
        DisconnectSessionByName(sessionName);
    }
}

void UTCPClientSubsystem::DisconnectSessionByName(const FString& sessionName)
{
    if (Sessions.Contains(sessionName))
    {
        UTCPSessionBase* Session = Sessions[sessionName];
        TCPClientController* controller = Session->GetController();
        
        if (Session->IsConnected())
        {
            controller->Disconnect(FString("Shutdown Manually"), true);
        }
        
        Sessions.Remove(sessionName);
        DeleteController(controller);
        Session->OnConnected.Unbind();
        Session->OnDisconnected.Unbind();
        Session->OnDestroy();
    }
}

UTCPSessionBase* UTCPClientSubsystem::GetSession(const FString& sessionName)
{
    if (Sessions.Contains(sessionName))
    {
        return Sessions[sessionName];
    }
    return nullptr;
}

void UTCPClientSubsystem::GetSessionLazy(const FString& sessionName, const FConnectedSessionDelegate& afterGetEvent)
{
    auto session = Sessions.Find(sessionName);
    if (session != nullptr && (*session)->IsConnected())
    {
        afterGetEvent.ExecuteIfBound(sessionName, (*session)->IsConnected(), (*session));
    }
    else
    {
        OnConnected.Add(afterGetEvent);
    }
}

void UTCPClientSubsystem::DeleteController(TCPClientController* controller)
{
    if (controller != nullptr)
    {
        delete controller;
        controller = nullptr;
    }
}

void UTCPClientSubsystem::ConnectedCallback(const FString& sessionName, bool success)
{
    if (!Sessions.Contains(sessionName))
        return;

    if (OnConnected.IsBound())
    {
        OnConnected.Broadcast(sessionName, success, Sessions[sessionName]);
        if (RegisteredConnectDelegates.Find(sessionName))
        {
            auto& registedDelegate = RegisteredConnectDelegates[sessionName];
            OnConnected.Remove(*registedDelegate);
            RegisteredConnectDelegates.Remove(sessionName);
        }
    }
}

void UTCPClientSubsystem::DisConnectedCallback(const FString& sessionName, bool normalShutdown)
{
	check(IsInGameThread()); // <-- NEU, extrem wichtig
    if (!Sessions.Contains(sessionName))
        return;

    auto session = Sessions[sessionName];
    
    if (OnDisconnected.IsBound())
    {
        OnDisconnected.Broadcast(sessionName, normalShutdown);
        if (RegisteredDisconnectDelegates.Find(sessionName))
        {
            auto& registedDelegate = RegisteredDisconnectDelegates[sessionName];
            OnDisconnected.Remove(*registedDelegate);
            RegisteredDisconnectDelegates.Remove(sessionName);
        }
    }
}

void UTCPClientSubsystem::GetDomainIpAddress(const FString& URL, TFunction<void(FString, bool)> OnComplete)
{
    /* code by -Kmack-
    * https://forums.unrealengine.com/t/how-to-get-host-by-name/296044/8
    */
    Async(EAsyncExecution::ThreadPool,
        [URL, OnComplete]()
        {
            ISocketSubsystem* const SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
            if (!SocketSubsystem)
            {
                AsyncTask(ENamedThreads::GameThread, [OnComplete]()
                    {
                        OnComplete(FString(), false);
                    });
                return;
            }

            // Neue API ab UE5.1
            FAddressInfoResult Result = SocketSubsystem->GetAddressInfo(
                *URL,
                nullptr,                          // kein ServiceName → nur Hostauflösung
                EAddressInfoFlags::Default,
                NAME_None                         // kein spezifisches Protokoll
            );

            if (Result.Results.Num() > 0)
            {
                // Nimm die erste Adresse
                TSharedRef<FInternetAddr> Addr = Result.Results[0].Address;

                FString IpString = Addr->ToString(false);

                UE_LOG(LogTemp, Warning, TEXT("Found IP address for URL <%s>: %s"), *URL, *IpString);

                AsyncTask(ENamedThreads::GameThread, [IpString, OnComplete]()
                    {
                        OnComplete(IpString, true);
                    });
            }
            else
            {
                AsyncTask(ENamedThreads::GameThread, [OnComplete]()
                    {
                        OnComplete(FString(), false);
                    });
            }
        }
    );
}


void UTCPClientSubsystem::Tick(float DeltaTime)
{

    for (auto& kvp : Sessions)
    {
        UTCPSessionBase* session = kvp.Value;
        TCPClientController* controller = session->GetController();
        if (controller == nullptr)
            continue;

        controller->CheckMessage();
        break;

    }
}

bool UTCPClientSubsystem::IsAllowedToTick() const
{
    return !IsTemplate();
}

ETickableTickType UTCPClientSubsystem::GetTickableTickType() const
{
    //{ return ETickableTickType::Always; }
    return IsTemplate() ? ETickableTickType::Never : FTickableGameObject::GetTickableTickType();
}

TStatId UTCPClientSubsystem::GetStatId() const
{
    return UObject::GetStatID();
}

bool UTCPClientSubsystem::IsTickable() const
{
    if (IsTemplate())
        return false;

    return true;
}