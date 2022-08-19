#include "EditorApplication_Win32.h"
#include "Engine_Win32.h"
#include "Resource.h"
#include "Applications/Shared/cmdParser/cmdParser.h"
#include "System/Imgui/ImguiStyle.h"
#include <tchar.h>
#include <windows.h>

#if EE_ENABLE_LPP
#include "LPP_API_x64_CPP.h"
#endif

//-------------------------------------------------------------------------

namespace EE
{
    EditorEngine::EditorEngine( TFunction<bool( EE::String const& error )>&& errorHandler )
        : Engine( eastl::forward<TFunction<bool( EE::String const& error )>&&>( errorHandler ) )
    {
        //m_module_engine_core.EnableImguiViewports();
    }

    void EditorEngine::CreateToolsUI()
    {
        auto pEditorUI = EE::New<EditorUI>();
        if ( m_editorStartupMap.IsValid() )
        {
            pEditorUI->SetStartupMap( m_editorStartupMap );
        }
        m_pToolsUI = pEditorUI;
    }

    //-------------------------------------------------------------------------

    EditorApplication::EditorApplication( HINSTANCE hInstance )
        : Win32Application( hInstance, "Esoterica Editor", IDI_EDITOR_ICON )
        , m_editorEngine( TFunction<bool( String const& error )>( [this] ( String const& error )-> bool  { return FatalError( error ); } ) )
    {}

    bool EditorApplication::ProcessCommandline( int32_t argc, char** argv )
    {
        cli::Parser cmdParser( argc, argv );
        cmdParser.set_optional<std::string>( "map", "map", "", "The startup map." );

        if ( !cmdParser.run() )
        {
            return FatalError( "Invalid command line arguments!" );
        }

        std::string const map = cmdParser.get<std::string>( "map" );
        if ( !map.empty() )
        {
            m_editorEngine.m_editorStartupMap = ResourcePath( map.c_str() );
        }

        return true;
    }

    bool EditorApplication::Initialize()
    {
        Int2 const windowDimensions( ( m_windowRect.right - m_windowRect.left ), ( m_windowRect.bottom - m_windowRect.top ) );
        if ( !m_editorEngine.Initialize( m_applicationNameNoWhitespace, windowDimensions ) )
        {
            return FatalError( "Failed to initialize engine" );
        }

        return true;
    }

    bool EditorApplication::Shutdown()
    {
        return m_editorEngine.Shutdown();
    }

    bool EditorApplication::ApplicationLoop()
    {
        // Uncomment for live editing of ImguiTheme
        //ImGuiX::Style::Apply();
        return m_editorEngine.Update();
    }

    //-------------------------------------------------------------------------

    LRESULT EditorApplication::WndProcess( HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam )
    {
        return DefaultEngineWindowProcessor( &m_editorEngine, hWnd, message, wParam, lParam );
    }
}

//-------------------------------------------------------------------------

int APIENTRY _tWinMain( HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, int nCmdShow )
{
    //-------------------------------------------------------------------------
    // Live++ Support
    //-------------------------------------------------------------------------

    #if EE_ENABLE_LPP
    auto lppAgent = lpp::LppCreateDefaultAgent( L"../../External/LivePP", L"" );
    lppAgent.EnableModule( lpp::LppGetCurrentModulePath(), lpp::LPP_MODULES_OPTION_ALL_IMPORT_MODULES );
    #endif

    //-------------------------------------------------------------------------

    EE::ApplicationGlobalState globalState;
    EE::EditorApplication editorApplication( hInstance );
    int32_t const result = editorApplication.Run( __argc, __argv );

    #if EE_ENABLE_LPP
    lpp::LppDestroyDefaultAgent( &lppAgent );
    #endif

    return result;
}