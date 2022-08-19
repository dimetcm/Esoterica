#pragma once
#include "System/Resource/ResourceID.h"
#include "System/Time/Time.h"
#include "System/Types/UUID.h"
#include "System/Time/Timestamp.h"

//-------------------------------------------------------------------------

namespace EE::Resource
{
    class CompilationRequest final
    {
        friend class ResourceServer;
        friend class ResourceServerWorker;

    public:

        enum class Status
        {
            Pending,
            Compiling,
            Succeeded,
            SucceededWithWarnings,
            Failed
        };

        enum class Origin
        {
            External,
            ManualCompile,
            FileWatcher,
            Package
        };

    public:

        // Get the client that requested this resource
        inline uint32_t GetClientID() const { EE_ASSERT( !IsInternalRequest() ); return m_clientID; }

        // Get the resource ID for this request
        inline ResourceID const& GetResourceID() const { return m_resourceID; }

        // Returns whether the request was externally requested (i.e. by a client) or internally requested (i.e. due to a file changing and being detected)
        inline bool IsInternalRequest() const { return m_origin != Origin::External; }

        // Status
        inline Status GetStatus() const { return m_status; }
        inline bool IsPending() const { return m_status == Status::Pending; }
        inline bool IsExecuting() const { return m_status == Status::Compiling; }
        inline bool HasSucceeded() const { return m_status == Status::Succeeded || m_status == Status::SucceededWithWarnings; }
        inline bool HasSucceededWithWarnings() const { return m_status == Status::SucceededWithWarnings; }
        inline bool HasFailed() const { return m_status == Status::Failed; }
        inline bool IsComplete() const { return HasSucceeded() || HasFailed(); }

        // Request Info
        inline char const* GetLog() const { return m_log.c_str(); }
        inline char const* GetCompilerArgs() const { return m_compilerArgs.c_str(); }
        inline FileSystem::Path const& GetSourceFilePath() const { return m_sourceFile; }
        inline FileSystem::Path const& GetDestinationFilePath() const { return m_destinationFile; }

        inline TimeStamp const& GetTimeRequested() const { return m_timeRequested; }

        inline Milliseconds GetCompilationElapsedTime() const
        {
            if ( m_status == Status::Pending )
            {
                return 0;
            }

            if ( !IsComplete() )
            {
                return Milliseconds( PlatformClock::GetTime() - m_compilationTimeStarted );
            }

            return Milliseconds( m_compilationTimeFinished - m_compilationTimeStarted );
        }

        inline Milliseconds GetUptoDateCheckElapsedTime() const
        {
            if ( m_status == Status::Pending )
            {
                return 0;
            }

            return Milliseconds( m_upToDateCheckTimeFinished - m_upToDateCheckTimeStarted );
        }

    public:

        uint32_t                            m_clientID = 0;
        ResourceID                          m_resourceID;
        int32_t                             m_compilerVersion = -1;
        uint64_t                            m_fileTimestamp = 0;
        uint64_t                            m_sourceTimestampHash = 0;
        FileSystem::Path                    m_sourceFile;
        FileSystem::Path                    m_destinationFile;
        String                              m_compilerArgs;

        TimeStamp                           m_timeRequested;
        Nanoseconds                         m_compilationTimeStarted = 0;
        Nanoseconds                         m_compilationTimeFinished = 0;
        Nanoseconds                         m_upToDateCheckTimeStarted = 0;
        Nanoseconds                         m_upToDateCheckTimeFinished = 0;

        String                              m_log;
        Status                              m_status = Status::Pending;
        Origin                              m_origin = Origin::External;
    };
}