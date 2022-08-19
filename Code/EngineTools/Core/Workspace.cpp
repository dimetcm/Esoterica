#include "Workspace.h"
#include "EngineTools/Resource/ResourceDescriptor.h"
#include "Engine/Render/DebugViews/DebugView_Render.h"
#include "Engine/Camera/DebugViews/DebugView_Camera.h"
#include "Engine/Camera/Components/Component_DebugCamera.h"
#include "Engine/Camera/Systems/EntitySystem_DebugCameraController.h"
#include "Engine/Entity/EntityWorld.h"
#include "Engine/ToolsUI/OrientationGuide.h"
#include "System/Imgui/ImguiStyle.h"
#include "System/TypeSystem/TypeRegistry.h"
#include "System/Resource/ResourceSystem.h"
#include "System/Serialization/TypeSerialization.h"
#include "System/Resource/ResourceRequesterID.h"

//-------------------------------------------------------------------------

namespace EE
{
    class ResourceDescriptorUndoableAction final : public IUndoableAction
    {
    public:

        ResourceDescriptorUndoableAction( TypeSystem::TypeRegistry const& typeRegistry, Workspace* pWorkspace )
            : m_typeRegistry( typeRegistry )
            , m_pWorkspace( pWorkspace )
        {
            EE_ASSERT( m_pWorkspace != nullptr );
            EE_ASSERT( m_pWorkspace->m_pDescriptor != nullptr );
        }

        virtual void Undo() override
        {
            Serialization::JsonArchiveReader typeReader;

            typeReader.ReadFromString( m_valueBefore.c_str() );
            auto const& document = typeReader.GetDocument();
            Serialization::ReadNativeType( m_typeRegistry, document, m_pWorkspace->m_pDescriptor );
            m_pWorkspace->ReadCustomDescriptorData( m_typeRegistry, document );
            m_pWorkspace->m_isDirty = true;
        }

        virtual void Redo() override
        {
            Serialization::JsonArchiveReader typeReader;

            typeReader.ReadFromString( m_valueAfter.c_str() );
            auto const& document = typeReader.GetDocument();
            Serialization::ReadNativeType( m_typeRegistry, document, m_pWorkspace->m_pDescriptor );
            m_pWorkspace->ReadCustomDescriptorData( m_typeRegistry, document );
            m_pWorkspace->m_isDirty = true;
        }

        void SerializeBeforeState()
        {
            Serialization::JsonArchiveWriter writer;

            auto pWriter = writer.GetWriter();
            pWriter->StartObject();
            Serialization::WriteNativeTypeContents( m_typeRegistry, m_pWorkspace->m_pDescriptor, *pWriter );
            m_pWorkspace->WriteCustomDescriptorData( m_typeRegistry, *pWriter );
            pWriter->EndObject();

            m_valueBefore.resize( writer.GetStringBuffer().GetSize() );
            memcpy( m_valueBefore.data(), writer.GetStringBuffer().GetString(), writer.GetStringBuffer().GetSize() );
        }

        void SerializeAfterState()
        {
            Serialization::JsonArchiveWriter writer;

            auto pWriter = writer.GetWriter();
            pWriter->StartObject();
            Serialization::WriteNativeTypeContents( m_typeRegistry, m_pWorkspace->m_pDescriptor, *pWriter );
            m_pWorkspace->WriteCustomDescriptorData( m_typeRegistry, *pWriter );
            pWriter->EndObject();

            m_valueAfter.resize( writer.GetStringBuffer().GetSize() );
            memcpy( m_valueAfter.data(), writer.GetStringBuffer().GetString(), writer.GetStringBuffer().GetSize() );

            m_pWorkspace->m_isDirty = true;
        }

    private:

        TypeSystem::TypeRegistry const&     m_typeRegistry;
        Workspace*                          m_pWorkspace = nullptr;
        String                              m_valueBefore;
        String                              m_valueAfter;
    };

    //-------------------------------------------------------------------------

    Workspace::Workspace( ToolsContext const* pToolsContext, EntityWorld* pWorld, ResourceID const& resourceID )
        : m_pToolsContext( pToolsContext )
        , m_pWorld( pWorld )
        , m_displayName( resourceID.GetFileNameWithoutExtension() )
        , m_descriptorID( resourceID )
        , m_descriptorPath( GetFileSystemPath( resourceID ) )
    {
        EE_ASSERT( m_pWorld != nullptr );
        EE_ASSERT( m_pToolsContext != nullptr && m_pToolsContext->IsValid() );
        EE_ASSERT( resourceID.IsValid() );

        m_ID = m_descriptorID.GetPathID();

        // Spawn Camera
        //-------------------------------------------------------------------------

        m_pCamera = EE::New<DebugCameraComponent>( StringID( "Camera Component" ) );
        m_pCamera->SetDefaultMoveSpeed( 5.0f );
        m_pCamera->ResetMoveSpeed();

        auto pEntity = EE::New<Entity>( StringID( "Camera" ) );
        pEntity->AddComponent( m_pCamera );
        pEntity->CreateSystem<DebugCameraController>();
        m_pWorld->GetPersistentMap()->AddEntity( pEntity );

        // Create descriptor property grid
        //-------------------------------------------------------------------------

        auto const PreDescEdit = [this] ( PropertyEditInfo const& info )
        {
            EE_ASSERT( m_pActiveUndoableAction == nullptr );
            EE_ASSERT( IsADescriptorWorkspace() && IsDescriptorLoaded() );
            BeginDescriptorModification();
        };

        auto const PostDescEdit = [this] ( PropertyEditInfo const& info )
        {
            EE_ASSERT( m_pActiveUndoableAction != nullptr );
            EE_ASSERT( IsADescriptorWorkspace() && IsDescriptorLoaded() );
            EndDescriptorModification();
        };

        m_pDescriptorPropertyGrid = EE::New<PropertyGrid>( m_pToolsContext );
        m_preEditEventBindingID = m_pDescriptorPropertyGrid->OnPreEdit().Bind( PreDescEdit );
        m_postEditEventBindingID = m_pDescriptorPropertyGrid->OnPostEdit().Bind( PostDescEdit );
    }

    Workspace::Workspace( ToolsContext const* pToolsContext, EntityWorld* pWorld, String const& displayName )
        : m_pToolsContext( pToolsContext )
        , m_pWorld( pWorld )
        , m_displayName( displayName )
    {
        EE_ASSERT( m_pWorld != nullptr );
        EE_ASSERT( m_pToolsContext != nullptr && m_pToolsContext->IsValid() );

        m_ID = Hash::GetHash32( displayName );

        // Spawn Camera
        //-------------------------------------------------------------------------

        m_pCamera = EE::New<DebugCameraComponent>( StringID( "Camera Component" ) );
        m_pCamera->SetDefaultMoveSpeed( 5.0f );
        m_pCamera->ResetMoveSpeed();

        auto pEntity = EE::New<Entity>( StringID( "Camera" ) );
        pEntity->AddComponent( m_pCamera );
        pEntity->CreateSystem<DebugCameraController>();
        m_pWorld->GetPersistentMap()->AddEntity( pEntity );
    }

    Workspace::~Workspace()
    {
        EE_ASSERT( m_requestedResources.empty() );
        EE_ASSERT( m_reloadingResources.empty() );
        EE_ASSERT( m_pDescriptor == nullptr );
        EE_ASSERT( m_pActiveUndoableAction == nullptr );

        if ( m_pDescriptorPropertyGrid != nullptr )
        {
            m_pDescriptorPropertyGrid->OnPreEdit().Unbind( m_preEditEventBindingID );
            m_pDescriptorPropertyGrid->OnPostEdit().Unbind( m_postEditEventBindingID );
            EE::Delete( m_pDescriptorPropertyGrid );
        }
    }

    void Workspace::Initialize( UpdateContext const& context )
    {
        SetDisplayName( m_displayName );
        m_viewportWindowID.sprintf( "Viewport##%u", GetID() );
        m_dockspaceID.sprintf( "Dockspace##%u", GetID() );
        m_descriptorWindowName.sprintf( "Descriptor##%u", GetID() );

        if ( IsADescriptorWorkspace() )
        {
            LoadDescriptor();
        }
    }

    void Workspace::Shutdown( UpdateContext const& context )
    {
        EE::Delete( m_pDescriptor );
    }

    void Workspace::SetDisplayName( String const& name )
    {
        m_displayName = name;
        m_pWorld->SetDebugName( m_displayName.c_str() );
        m_workspaceWindowID.sprintf( "%s###window%u", m_displayName.c_str(), GetID() );
    }

    Drawing::DrawContext Workspace::GetDrawingContext()
    {
        return m_pWorld->GetDebugDrawingSystem()->GetDrawingContext();
    }

    //-------------------------------------------------------------------------

    void Workspace::InitializeDockingLayout( ImGuiID dockspaceID ) const
    {
        ImGui::DockBuilderDockWindow( m_descriptorWindowName.c_str(), dockspaceID );
    }

    void Workspace::DrawWorkspaceToolbar( UpdateContext const& context )
    {
        if ( HasWorkspaceToolbarDefaultItems() )
        {
            bool const isSavingAllowed = AlwaysAllowSaving() || IsDirty();

            ImGui::BeginDisabled( !isSavingAllowed );
            if ( ImGui::MenuItem( EE_ICON_CONTENT_SAVE"##Save" ) )
            {
                Save();
            }
            ImGuiX::ItemTooltip( "Save" );
            ImGui::EndDisabled();

            ImGui::BeginDisabled( !CanUndo() );
            if ( ImGui::MenuItem( EE_ICON_UNDO"##Undo" ) )
            {
                Undo();
            }
            ImGuiX::ItemTooltip( "Undo" );
            ImGui::EndDisabled();

            ImGui::BeginDisabled( !CanRedo() );
            if ( ImGui::MenuItem( EE_ICON_REDO"##Redo" ) )
            {
                Redo();
            }
            ImGuiX::ItemTooltip( "Redo" );
            ImGui::EndDisabled();
        }

        //-------------------------------------------------------------------------

        if ( IsADescriptorWorkspace() )
        {
            if ( ImGui::MenuItem( EE_ICON_CONTENT_COPY"##Copy Path" ) )
            {
                ImGui::SetClipboardText( m_descriptorID.c_str() );
            }
            ImGuiX::ItemTooltip( "Copy Resource Path" );
        }

        //-------------------------------------------------------------------------

        DrawWorkspaceToolbarItems( context );
    }

    //-------------------------------------------------------------------------

    bool Workspace::BeginViewportToolbarGroup( char const* pGroupID, ImVec2 groupSize, ImVec2 const& padding )
    {
        ImGui::SameLine();

        ImGui::PushStyleColor( ImGuiCol_ChildBg, ImGuiX::Style::s_colorGray5.Value );
        ImGui::PushStyleColor( ImGuiCol_Header, ImGuiX::Style::s_colorGray5.Value );
        ImGui::PushStyleColor( ImGuiCol_FrameBg, ImGuiX::Style::s_colorGray5.Value );
        ImGui::PushStyleColor( ImGuiCol_FrameBgHovered, ImGuiX::Style::s_colorGray4.Value );
        ImGui::PushStyleColor( ImGuiCol_FrameBgActive, ImGuiX::Style::s_colorGray3.Value );

        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, padding );
        ImGui::PushStyleVar( ImGuiStyleVar_ChildRounding, 4.0f );

        // Adjust "use available" height to default toolbar height
        if ( groupSize.y <= 0 )
        {
            groupSize.y = ImGui::GetFrameHeight();
        }

        return ImGui::BeginChild( pGroupID, groupSize, false, ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar );
    }

    void Workspace::EndViewportToolbarGroup()
    {
        ImGui::EndChild();
        ImGui::PopStyleVar( 2 );
        ImGui::PopStyleColor( 5 );

        ImGui::SameLine();
    }

    void Workspace::DrawViewportToolbar( UpdateContext const& context, Render::Viewport const* pViewport )
    {
        ImGui::SetNextItemWidth( 48 );
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 5, 5 ) );
        if ( ImGui::BeginCombo( "##RenderingOptions", EE_ICON_EYE, ImGuiComboFlags_HeightLarge ) )
        {
            Render::RenderDebugView::DrawRenderVisualizationModesMenu( GetWorld() );
            ImGui::EndCombo();
        }
        ImGuiX::ItemTooltip( "Render Modes" );
        ImGui::PopStyleVar();
        ImGui::SameLine();

        //-------------------------------------------------------------------------

        ImGui::SetNextItemWidth( 48 );
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 5, 5 ) );
        if ( ImGui::BeginCombo( "##CameraOptions", EE_ICON_CCTV, ImGuiComboFlags_HeightLarge ) )
        {
            CameraDebugView::DrawDebugCameraOptions( GetWorld() );

            ImGui::EndCombo();
        }
        ImGuiX::ItemTooltip( "Camera Options" );
        ImGui::PopStyleVar();
        ImGui::SameLine();

        //-------------------------------------------------------------------------

        if ( HasViewportToolbarTimeControls() )
        {
            if ( BeginViewportToolbarGroup( "TimeControls", ImVec2( 200, 0 ), ImVec2( 2, 1 ) ) )
            {
                ImGuiX::ScopedFont sf( ImGuiX::Font::Small );

                ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 3 ) );

                // Play/Pause
                if ( m_pWorld->IsPaused() )
                {
                    if ( ImGui::Button( EE_ICON_PLAY"##ResumeWorld", ImVec2( 20, 0 ) ) )
                    {
                        SetWorldPaused( false );
                    }
                    ImGuiX::ItemTooltip( "Resume" );
                }
                else
                {
                    if ( ImGui::Button( EE_ICON_PAUSE"##PauseWorld", ImVec2( 20, 0 ) ) )
                    {
                        SetWorldPaused( true );
                    }
                    ImGuiX::ItemTooltip( "Pause" );
                }

                // Step
                ImGui::SameLine( 0, 0 );
                ImGui::BeginDisabled( !m_pWorld->IsPaused() );
                if ( ImGui::Button( EE_ICON_ARROW_RIGHT_BOLD"##StepFrame", ImVec2( 20, 0 ) ) )
                {
                    m_pWorld->RequestTimeStep();
                }
                ImGuiX::ItemTooltip( "Step Frame" );
                ImGui::EndDisabled();

                // Slider
                ImGui::SameLine( 0, 0 );
                ImGui::SetNextItemWidth( 136 );
                float newTimeScale = m_worldTimeScale;
                if ( ImGui::SliderFloat( "##TimeScale", &newTimeScale, 0.1f, 3.5f, "%.2f", ImGuiSliderFlags_NoInput ) )
                {
                    SetWorldTimeScale( newTimeScale );
                }
                ImGuiX::ItemTooltip( "Time Scale" );

                // Reset
                ImGui::SameLine( 0, 0 );
                if ( ImGui::Button( EE_ICON_UPDATE"##ResetTimeScale", ImVec2( 20, 0 ) ) )
                {
                    ResetWorldTimeScale();
                }
                ImGuiX::ItemTooltip( "Reset TimeScale" );

                ImGui::PopStyleVar();
            }
            EndViewportToolbarGroup();
        }

        //-------------------------------------------------------------------------

        DrawViewportToolbarItems( context, pViewport );
    }

    bool Workspace::DrawViewport( UpdateContext const& context, ViewportInfo const& viewportInfo, ImGuiWindowClass* pWindowClass )
    {
        EE_ASSERT( viewportInfo.m_pViewportRenderTargetTexture != nullptr && viewportInfo.m_retrievePickingID != nullptr );

        m_isViewportFocused = false;
        m_isViewportHovered = false;

        auto pWorld = GetWorld();
        Render::Viewport* pViewport = pWorld->GetViewport();

        // Create viewport window
        ImGuiWindowFlags const viewportWindowFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavInputs;
        ImGui::SetNextWindowClass( pWindowClass );
        ImGui::SetNextWindowSizeConstraints( ImVec2( 128, 128 ), ImVec2( FLT_MAX, FLT_MAX ) );
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0, 0 ) );
        if ( ImGui::Begin( GetViewportWindowID(), nullptr, viewportWindowFlags ) )
        {
            m_isViewportFocused = ImGui::IsWindowFocused();
            m_isViewportHovered = ImGui::IsWindowHovered();

            ImGuiStyle const& style = ImGui::GetStyle();
            ImVec2 const viewportSize( Math::Max( ImGui::GetContentRegionAvail().x, 64.0f ), Math::Max( ImGui::GetContentRegionAvail().y, 64.0f ) );

            ImVec2 const windowPos = ImGui::GetWindowPos();

            // Switch focus based on mouse input
            //-------------------------------------------------------------------------

            if ( m_isViewportHovered )
            {
                if ( ImGui::IsMouseClicked( ImGuiMouseButton_Left ) || ImGui::IsMouseClicked( ImGuiMouseButton_Right ) || ImGui::IsMouseClicked( ImGuiMouseButton_Middle ) )
                {
                    ImGui::SetWindowFocus();
                    m_isViewportFocused = true;
                }
            }

            // Update engine viewport dimensions
            //-------------------------------------------------------------------------

            Math::Rectangle const viewportRect( Float2::Zero, viewportSize );
            pViewport->Resize( viewportRect );

            // Draw 3D scene
            //-------------------------------------------------------------------------

            ImVec2 const viewportImageCursorPos = ImGui::GetCursorPos();
            ImGui::Image( viewportInfo.m_pViewportRenderTargetTexture, viewportSize );

            if ( ImGui::BeginDragDropTarget() )
            {
                OnDragAndDrop( pViewport );
                ImGui::EndDragDropTarget();
            }

            // Draw overlay elements
            //-------------------------------------------------------------------------

            ImGui::SetCursorPos( style.WindowPadding );
            DrawViewportOverlayElements( context, pViewport );

            if ( HasViewportOrientationGuide() )
            {
                ImGuiX::OrientationGuide::Draw( ImGui::GetWindowPos() + viewportSize - ImVec2( ImGuiX::OrientationGuide::GetWidth() + 4, ImGuiX::OrientationGuide::GetWidth() + 4 ), *pViewport );
            }

            // Draw viewport toolbar
            //-------------------------------------------------------------------------

            if ( HasViewportToolbar() )
            {
                ImGui::SetCursorPos( ImGui::GetWindowContentRegionMin() + ImGui::GetStyle().ItemSpacing );
                DrawViewportToolbar( context, pViewport );
            }

            // Handle picking
            //-------------------------------------------------------------------------

            if ( m_isViewportHovered && !ImGui::IsAnyItemHovered() )
            {
                if ( ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
                {
                    ImVec2 const mousePos = ImGui::GetMousePos();
                    if ( mousePos.x != FLT_MAX && mousePos.y != FLT_MAX )
                    {
                        ImVec2 const mousePosWithinViewportImage = ( mousePos - windowPos ) - viewportImageCursorPos;
                        Int2 const pixelCoords = Int2( Math::RoundToInt( mousePosWithinViewportImage.x ), Math::RoundToInt( mousePosWithinViewportImage.y ) );
                        Render::PickingID const pickingID = viewportInfo.m_retrievePickingID( pixelCoords );
                        if ( pickingID.IsSet() )
                        {
                            OnMousePick( pickingID );
                        }
                    }
                }
            }

            // Handle being docked
            //-------------------------------------------------------------------------

            if ( auto pDockNode = ImGui::GetWindowDockNode() )
            {
                pDockNode->LocalFlags = 0;
                pDockNode->LocalFlags |= ImGuiDockNodeFlags_NoDockingOverMe;
                pDockNode->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();

        //-------------------------------------------------------------------------

        return m_isViewportFocused;
    }

    //-------------------------------------------------------------------------

    void Workspace::SetCameraUpdateEnabled( bool isEnabled )
    {
        m_pCamera->SetEnabled( isEnabled );
    }

    void Workspace::ResetCameraView()
    {
        EE_ASSERT( m_pCamera != nullptr );
        m_pCamera->ResetView();
    }

    void Workspace::FocusCameraView( Entity* pTarget )
    {
        if ( !pTarget->IsSpatialEntity() )
        {
            ResetCameraView();
            return;
        }

        EE_ASSERT( m_pCamera != nullptr );
        OBB const worldBounds = pTarget->GetCombinedWorldBounds();
        m_pCamera->FocusOn( worldBounds );
    }

    void Workspace::SetViewportCameraSpeed( float cameraSpeed )
    {
        EE_ASSERT( m_pCamera != nullptr );
        m_pCamera->SetMoveSpeed( cameraSpeed );
    }

    void Workspace::SetViewportCameraTransform( Transform const& cameraTransform )
    {
        EE_ASSERT( m_pCamera != nullptr );
        m_pCamera->SetWorldTransform( cameraTransform );
    }

    Transform Workspace::GetViewportCameraTransform() const
    {
        EE_ASSERT( m_pCamera != nullptr );
        return m_pCamera->GetWorldTransform();
    }

    //-------------------------------------------------------------------------

    void Workspace::SetWorldPaused( bool newPausedState )
    {
        bool const currentPausedState = m_pWorld->IsPaused();

        if ( currentPausedState == newPausedState )
        {
            return;
        }

        //-------------------------------------------------------------------------

        if ( m_pWorld->IsPaused() )
        {
            m_pWorld->SetTimeScale( m_worldTimeScale );
        }
        else // Pause
        {
            m_worldTimeScale = m_pWorld->GetTimeScale();
            m_pWorld->SetTimeScale( -1.0f );
        }
    }

    void Workspace::SetWorldTimeScale( float newTimeScale )
    {
        m_worldTimeScale = Math::Clamp( newTimeScale, 0.1f, 3.5f );
        if ( !m_pWorld->IsPaused() )
        {
            m_pWorld->SetTimeScale( m_worldTimeScale );
        }
    }

    void Workspace::ResetWorldTimeScale()
    {
        m_worldTimeScale = 1.0f;
        if ( !m_pWorld->IsPaused() )
        {
            m_pWorld->SetTimeScale( 1.0f );
        }
    }

    //-------------------------------------------------------------------------

    void Workspace::LoadResource( Resource::ResourcePtr* pResourcePtr )
    {
        EE_ASSERT( pResourcePtr != nullptr && pResourcePtr->IsUnloaded() );
        EE_ASSERT( !VectorContains( m_requestedResources, pResourcePtr ) );
        m_requestedResources.emplace_back( pResourcePtr );
        m_pToolsContext->m_pResourceSystem->LoadResource( *pResourcePtr, Resource::ResourceRequesterID( Resource::ResourceRequesterID::s_toolsRequestID ) );
    }

    void Workspace::UnloadResource( Resource::ResourcePtr* pResourcePtr )
    {
        EE_ASSERT( !pResourcePtr->IsUnloaded() );
        EE_ASSERT( VectorContains( m_requestedResources, pResourcePtr ) );
        m_pToolsContext->m_pResourceSystem->UnloadResource( *pResourcePtr, Resource::ResourceRequesterID( Resource::ResourceRequesterID::s_toolsRequestID ) );
        m_requestedResources.erase_first_unsorted( pResourcePtr );
    }

    void Workspace::AddEntityToWorld( Entity* pEntity )
    {
        EE_ASSERT( pEntity != nullptr && !pEntity->IsAddedToMap() );
        EE_ASSERT( !VectorContains( m_addedEntities, pEntity ) );
        m_addedEntities.emplace_back( pEntity );
        m_pWorld->GetPersistentMap()->AddEntity( pEntity );
    }

    void Workspace::RemoveEntityFromWorld( Entity* pEntity )
    {
        EE_ASSERT( pEntity != nullptr && pEntity->GetMapID() == m_pWorld->GetPersistentMap()->GetID() );
        EE_ASSERT( VectorContains( m_addedEntities, pEntity ) );
        m_pWorld->GetPersistentMap()->RemoveEntity( pEntity );
        m_addedEntities.erase_first_unsorted( pEntity );
    }

    void Workspace::DestroyEntityInWorld( Entity*& pEntity )
    {
        EE_ASSERT( pEntity != nullptr && pEntity->GetMapID() == m_pWorld->GetPersistentMap()->GetID() );
        EE_ASSERT( VectorContains( m_addedEntities, pEntity ) );
        m_pWorld->GetPersistentMap()->DestroyEntity( pEntity );
        m_addedEntities.erase_first_unsorted( pEntity );
        pEntity = nullptr;
    }

    //-------------------------------------------------------------------------

    bool Workspace::Save()
    {
        // Save Descriptor
        if ( IsADescriptorWorkspace() )
        {
            EE_ASSERT( m_descriptorPath.IsFilePath() );
            EE_ASSERT( m_pDescriptor != nullptr );
            EE_ASSERT( m_pDescriptorPropertyGrid != nullptr );

            // Serialize descriptor
            //-------------------------------------------------------------------------

            Serialization::JsonArchiveWriter descriptorWriter;
            auto pWriter = descriptorWriter.GetWriter();

            pWriter->StartObject();
            Serialization::WriteNativeTypeContents( *m_pToolsContext->m_pTypeRegistry, m_pDescriptor, *pWriter );
            WriteCustomDescriptorData( *m_pToolsContext->m_pTypeRegistry, *pWriter );
            pWriter->EndObject();

            // Save to file
            //-------------------------------------------------------------------------

            if ( descriptorWriter.WriteToFile( m_descriptorPath ) )
            {
                m_pDescriptorPropertyGrid->ClearDirty();
                m_isDirty = false;
                return true;
            }
            else
            {
                return false;
            }
        }

        return true;
    }

    //-------------------------------------------------------------------------

    void Workspace::BeginDescriptorModification()
    {
        if ( m_beginModificationCallCount == 0 )
        {
            auto pUndoableAction = EE::New<ResourceDescriptorUndoableAction>( *m_pToolsContext->m_pTypeRegistry, this );
            pUndoableAction->SerializeBeforeState();
            m_pActiveUndoableAction = pUndoableAction;
        }
        m_beginModificationCallCount++;
    }

    void Workspace::EndDescriptorModification()
    {
        EE_ASSERT( m_beginModificationCallCount > 0 );
        EE_ASSERT( m_pActiveUndoableAction != nullptr );

        m_beginModificationCallCount--;

        if ( m_beginModificationCallCount == 0 )
        {
            auto pUndoableAction = static_cast<ResourceDescriptorUndoableAction*>( m_pActiveUndoableAction );
            pUndoableAction->SerializeAfterState();
            m_undoStack.RegisterAction( pUndoableAction );
            m_pActiveUndoableAction = nullptr;
            m_isDirty = true;
        }
    }

    //-------------------------------------------------------------------------

    void Workspace::PostUndoRedo( UndoStack::Operation operation, IUndoableAction const* pAction )
    {
        if ( m_pDescriptorPropertyGrid != nullptr )
        {
            m_pDescriptorPropertyGrid->MarkDirty();
        }
    }

    void Workspace::Undo()
    {
        PreUndoRedo( UndoStack::Operation::Undo ); auto pAction = m_undoStack.Undo(); PostUndoRedo( UndoStack::Operation::Undo, pAction );
    }

    void Workspace::Redo()
    {
        PreUndoRedo( UndoStack::Operation::Redo ); auto pAction = m_undoStack.Redo(); PostUndoRedo( UndoStack::Operation::Redo, pAction );
    }

    //-------------------------------------------------------------------------

    void Workspace::BeginHotReload( TVector<Resource::ResourceRequesterID> const& usersToBeReloaded, TVector<ResourceID> const& resourcesToBeReloaded )
    {
        // Destroy descriptor if the resource we are operating on was modified
        if ( IsADescriptorWorkspace() )
        {
            if ( VectorContains( resourcesToBeReloaded, m_descriptorID ) )
            {
                EE::Delete( m_pDescriptor );
            }
        }

        // Unload necessary resources
        for ( auto& pLoadedResource : m_requestedResources )
        {
            if ( pLoadedResource->IsUnloaded() )
            {
                continue;
            }

            // Check resource and install dependencies to see if we need to unload it
            bool shouldUnload = VectorContains( resourcesToBeReloaded, pLoadedResource->GetResourceID() );
            if ( !shouldUnload )
            {
                for ( ResourceID const& installDependency : pLoadedResource->GetInstallDependencies() )
                {
                    if ( VectorContains( resourcesToBeReloaded, installDependency ) )
                    {
                        shouldUnload = true;
                        break;
                    }
                }
            }

            // Request unload and track the resource we need to reload
            if ( shouldUnload )
            {
                m_pToolsContext->m_pResourceSystem->UnloadResource( *pLoadedResource, Resource::ResourceRequesterID( Resource::ResourceRequesterID::s_toolsRequestID ) );
                m_reloadingResources.emplace_back( pLoadedResource );
            }
        }
    }

    void Workspace::EndHotReload()
    {
        // Load all unloaded resources
        for ( auto& pReloadedResource : m_reloadingResources )
        {
            m_pToolsContext->m_pResourceSystem->LoadResource( *pReloadedResource, Resource::ResourceRequesterID( Resource::ResourceRequesterID::s_toolsRequestID ) );
        }
        m_reloadingResources.clear();

        // Reload the descriptor if needed
        if ( IsADescriptorWorkspace() && !IsDescriptorLoaded() )
        {
            LoadDescriptor();
        }
    }

    //-------------------------------------------------------------------------

    void Workspace::LoadDescriptor()
    {
        EE_ASSERT( IsADescriptorWorkspace() );
        EE_ASSERT( m_pDescriptor == nullptr );
        EE_ASSERT( m_pDescriptorPropertyGrid != nullptr );

        Serialization::JsonArchiveReader archive;
        if ( !archive.ReadFromFile( m_descriptorPath ) )
        {
            EE_LOG_ERROR( "Tools", "Resource Workspace", "Failed to read resource descriptor file: %s", m_descriptorPath.c_str() );
            return;
        }

        auto const& document = archive.GetDocument();
        m_pDescriptor = Cast<Resource::ResourceDescriptor>( Serialization::CreateAndReadNativeType( *m_pToolsContext->m_pTypeRegistry, document ) );
        m_pDescriptorPropertyGrid->SetTypeToEdit( m_pDescriptor );

        ReadCustomDescriptorData( *m_pToolsContext->m_pTypeRegistry, document );
    }

    bool Workspace::DrawDescriptorEditorWindow( UpdateContext const& context, ImGuiWindowClass* pWindowClass, bool isSeparateWindow )
    {
        EE_ASSERT( IsADescriptorWorkspace() );
        EE_ASSERT( m_pDescriptorPropertyGrid != nullptr );

        bool hasFocus = false;
        ImGui::SetNextWindowClass( pWindowClass );
        if ( ImGui::Begin( m_descriptorWindowName.c_str() ) )
        {
            if ( !isSeparateWindow )
            {
                if ( auto pDockNode = ImGui::GetWindowDockNode() )
                {
                    pDockNode->LocalFlags |= ImGuiDockNodeFlags_HiddenTabBar;
                }
            }

            //-------------------------------------------------------------------------

            if ( m_pDescriptor == nullptr )
            {
                ImGui::Text( "Failed to load descriptor!" );
            }
            else
            {
                if ( !isSeparateWindow )
                {
                    ImGuiX::ScopedFont sf( ImGuiX::Font::Medium );
                    ImGui::Text( "Descriptor: %s", m_descriptorID.c_str() );

                    ImGui::BeginDisabled( !m_pDescriptorPropertyGrid->IsDirty() );
                    if ( ImGuiX::ColoredButton( ImGuiX::ConvertColor( Colors::ForestGreen ), ImGuiX::ConvertColor( Colors::White ), EE_ICON_CONTENT_SAVE" Save", ImVec2( -1, 0 ) ) )
                    {
                        Save();
                    }
                    ImGui::EndDisabled();
                }

                m_pDescriptorPropertyGrid->DrawGrid();
            }

            hasFocus = ImGui::IsWindowFocused( ImGuiFocusedFlags_RootAndChildWindows );
        }
        ImGui::End();

        return hasFocus;
    }

    //-------------------------------------------------------------------------

    void Workspace::Update( UpdateContext const& context, ImGuiWindowClass* pWindowClass, bool isFocused )
    {
        DrawDescriptorEditorWindow( context, pWindowClass, true );
    }

    void Workspace::InternalSharedUpdate( UpdateContext const& context, ImGuiWindowClass* pWindowClass, bool isFocused )
    {
        if ( isFocused )
        {
            auto& IO = ImGui::GetIO();
            if ( IO.KeyCtrl && ImGui::IsKeyPressed( ImGuiKey_Z ) )
            {
                if ( CanUndo() )
                {
                    Undo();
                }
            }

            if ( IO.KeyCtrl && ImGui::IsKeyPressed( ImGuiKey_Y ) )
            {
                if ( CanRedo() )
                {
                    Redo();
                }
            }
        }
    }
}

//-------------------------------------------------------------------------

namespace EE
{
    EE_DEFINE_GLOBAL_REGISTRY( ResourceWorkspaceFactory );

    //-------------------------------------------------------------------------

    bool ResourceWorkspaceFactory::CanCreateWorkspace( ToolsContext const* pToolsContext, ResourceID const& resourceID )
    {
        EE_ASSERT( resourceID.IsValid() );
        auto resourceTypeID = resourceID.GetResourceTypeID();
        EE_ASSERT( resourceTypeID.IsValid() );

        // Check if we have a custom workspace for this type
        //-------------------------------------------------------------------------

        auto pCurrentFactory = s_pHead;
        while ( pCurrentFactory != nullptr )
        {
            if ( resourceTypeID == pCurrentFactory->GetSupportedResourceTypeID() )
            {
                return true;
            }

            pCurrentFactory = pCurrentFactory->GetNextItem();
        }

        // Check if a descriptor type
        //-------------------------------------------------------------------------

        auto const resourceDescriptorTypes = pToolsContext->m_pTypeRegistry->GetAllDerivedTypes( Resource::ResourceDescriptor::GetStaticTypeID(), false, false );
        for ( auto pResourceDescriptorTypeInfo : resourceDescriptorTypes )
        {
            auto pDefaultInstance = Cast<Resource::ResourceDescriptor>( pResourceDescriptorTypeInfo->GetDefaultInstance() );
            if ( pDefaultInstance->GetCompiledResourceTypeID() == resourceID.GetResourceTypeID() )
            {
                return true;
            }
        }

        //-------------------------------------------------------------------------

        return false;
    }

    Workspace* ResourceWorkspaceFactory::CreateWorkspace( ToolsContext const* pToolsContext, EntityWorld* pWorld, ResourceID const& resourceID )
    {
        EE_ASSERT( resourceID.IsValid() );
        auto resourceTypeID = resourceID.GetResourceTypeID();
        EE_ASSERT( resourceTypeID.IsValid() );

        // Check if we have a custom workspace for this type
        //-------------------------------------------------------------------------

        auto pCurrentFactory = s_pHead;
        while ( pCurrentFactory != nullptr )
        {
            if ( resourceTypeID == pCurrentFactory->GetSupportedResourceTypeID() )
            {
                return pCurrentFactory->CreateWorkspaceInternal( pToolsContext, pWorld, resourceID );
            }

            pCurrentFactory = pCurrentFactory->GetNextItem();
        }

        // Create generic descriptor workspace
        //-------------------------------------------------------------------------

        auto const resourceDescriptorTypes = pToolsContext->m_pTypeRegistry->GetAllDerivedTypes( Resource::ResourceDescriptor::GetStaticTypeID(), false, false );
        for ( auto pResourceDescriptorTypeInfo : resourceDescriptorTypes )
        {
            auto pDefaultInstance = Cast<Resource::ResourceDescriptor>( pResourceDescriptorTypeInfo->GetDefaultInstance() );
            if ( pDefaultInstance->GetCompiledResourceTypeID() == resourceID.GetResourceTypeID() )
            {
                return EE::New<Workspace>( pToolsContext, pWorld, resourceID );
            }
        }

        EE_UNREACHABLE_CODE();
        return nullptr;
    }
}