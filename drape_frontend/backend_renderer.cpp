#include "drape_frontend/gui/drape_gui.hpp"

#include "drape_frontend/backend_renderer.hpp"
#include "drape_frontend/batchers_pool.hpp"
#include "drape_frontend/drape_api_builder.hpp"
#include "drape_frontend/drape_measurer.hpp"
#include "drape_frontend/gps_track_shape.hpp"
#include "drape_frontend/map_shape.hpp"
#include "drape_frontend/message_subclasses.hpp"
#include "drape_frontend/read_manager.hpp"
#include "drape_frontend/route_builder.hpp"
#include "drape_frontend/user_mark_shapes.hpp"
#include "drape_frontend/visual_params.hpp"

#include "indexer/scales.hpp"

#include "drape/texture_manager.hpp"

#include "platform/platform.hpp"

#include "base/logging.hpp"

#include "std/bind.hpp"

namespace df
{

BackendRenderer::BackendRenderer(Params && params)
  : BaseRenderer(ThreadsCommutator::ResourceUploadThread, params)
  , m_model(params.m_model)
  , m_readManager(make_unique_dp<ReadManager>(params.m_commutator, m_model,
                                              params.m_allow3dBuildings, params.m_trafficEnabled))
  , m_trafficGenerator(make_unique_dp<TrafficGenerator>(bind(&BackendRenderer::FlushTrafficRenderData, this, _1)))
  , m_requestedTiles(params.m_requestedTiles)
  , m_updateCurrentCountryFn(params.m_updateCurrentCountryFn)
{
#ifdef DEBUG
  m_isTeardowned = false;
#endif

  TrafficGenerator::SetSimplifiedColorSchemeEnabled(params.m_simplifiedTrafficColors);

  ASSERT(m_updateCurrentCountryFn != nullptr, ());

  m_routeBuilder = make_unique_dp<RouteBuilder>([this](drape_ptr<RouteData> && routeData)
  {
    m_commutator->PostMessage(ThreadsCommutator::RenderThread,
                              make_unique_dp<FlushRouteMessage>(move(routeData)),
                              MessagePriority::Normal);
  }, [this](drape_ptr<RouteSignData> && routeSignData)
  {
    m_commutator->PostMessage(ThreadsCommutator::RenderThread,
                              make_unique_dp<FlushRouteSignMessage>(move(routeSignData)),
                              MessagePriority::Normal);
  }, [this](drape_ptr<RouteArrowsData> && routeArrowsData)
  {
    m_commutator->PostMessage(ThreadsCommutator::RenderThread,
                              make_unique_dp<FlushRouteArrowsMessage>(move(routeArrowsData)),
                              MessagePriority::Normal);
  });

  StartThread();
}

BackendRenderer::~BackendRenderer()
{
  ASSERT(m_isTeardowned, ());
}

void BackendRenderer::Teardown()
{
  StopThread();
#ifdef DEBUG
  m_isTeardowned = true;
#endif
}

unique_ptr<threads::IRoutine> BackendRenderer::CreateRoutine()
{
  return make_unique<Routine>(*this);
}

void BackendRenderer::RecacheGui(gui::TWidgetsInitInfo const & initInfo, bool needResetOldGui)
{
  drape_ptr<gui::LayerRenderer> layerRenderer = m_guiCacher.RecacheWidgets(initInfo, m_texMng);
  drape_ptr<Message> outputMsg = make_unique_dp<GuiLayerRecachedMessage>(move(layerRenderer), needResetOldGui);
  m_commutator->PostMessage(ThreadsCommutator::RenderThread, move(outputMsg), MessagePriority::Normal);
}

#ifdef RENRER_DEBUG_INFO_LABELS
void BackendRenderer::RecacheDebugLabels()
{
  drape_ptr<gui::LayerRenderer> layerRenderer = m_guiCacher.RecacheDebugLabels(m_texMng);
  drape_ptr<Message> outputMsg = make_unique_dp<GuiLayerRecachedMessage>(move(layerRenderer), false);
  m_commutator->PostMessage(ThreadsCommutator::RenderThread, move(outputMsg), MessagePriority::Normal);
}
#endif

void BackendRenderer::RecacheChoosePositionMark()
{
  drape_ptr<gui::LayerRenderer> layerRenderer = m_guiCacher.RecacheChoosePositionMark(m_texMng);
  drape_ptr<Message> outputMsg = make_unique_dp<GuiLayerRecachedMessage>(move(layerRenderer), false);
  m_commutator->PostMessage(ThreadsCommutator::RenderThread, move(outputMsg), MessagePriority::Normal);
}

void BackendRenderer::AcceptMessage(ref_ptr<Message> message)
{
  switch (message->GetType())
  {
  case Message::UpdateReadManager:
    {
      TTilesCollection tiles = m_requestedTiles->GetTiles();
      if (!tiles.empty())
      {
        ScreenBase screen;
        bool have3dBuildings;
        bool needRegenerateTraffic;
        m_requestedTiles->GetParams(screen, have3dBuildings, needRegenerateTraffic);
        m_readManager->UpdateCoverage(screen, have3dBuildings, needRegenerateTraffic, tiles, m_texMng);
        m_updateCurrentCountryFn(screen.ClipRect().Center(), (*tiles.begin()).m_zoomLevel);
      }
      break;
    }

  case Message::InvalidateReadManagerRect:
    {
      ref_ptr<InvalidateReadManagerRectMessage> msg = message;
      if (msg->NeedInvalidateAll())
        m_readManager->InvalidateAll();
      else
        m_readManager->Invalidate(msg->GetTilesForInvalidate());
      break;
    }

  case Message::ShowChoosePositionMark:
    {
      RecacheChoosePositionMark();
      break;
    }

  case Message::GuiRecache:
    {
      ref_ptr<GuiRecacheMessage> msg = message;
      RecacheGui(msg->GetInitInfo(), msg->NeedResetOldGui());

#ifdef RENRER_DEBUG_INFO_LABELS
      RecacheDebugLabels();
#endif
      break;
    }

  case Message::GuiLayerLayout:
    {
      ref_ptr<GuiLayerLayoutMessage> msg = message;
      m_commutator->PostMessage(ThreadsCommutator::RenderThread,
                                make_unique_dp<GuiLayerLayoutMessage>(msg->AcceptLayoutInfo()),
                                MessagePriority::Normal);
      break;
    }

  case Message::TileReadStarted:
    {
      ref_ptr<TileReadStartMessage> msg = message;
      m_batchersPool->ReserveBatcher(msg->GetKey());
      break;
    }

  case Message::TileReadEnded:
    {
      ref_ptr<TileReadEndMessage> msg = message;
      m_batchersPool->ReleaseBatcher(msg->GetKey());
      break;
    }

  case Message::FinishTileRead:
    {
      ref_ptr<FinishTileReadMessage> msg = message;
      m_commutator->PostMessage(ThreadsCommutator::RenderThread,
                                make_unique_dp<FinishTileReadMessage>(msg->MoveTiles()),
                                MessagePriority::Normal);
      break;
    }

  case Message::FinishReading:
    {
      TOverlaysRenderData overlays;
      overlays.swap(m_overlays);
      if (!overlays.empty())
      {
        m_commutator->PostMessage(ThreadsCommutator::RenderThread,
                                  make_unique_dp<FlushOverlaysMessage>(move(overlays)),
                                  MessagePriority::Normal);
      }
      break;
    }

  case Message::MapShapesRecache:
    {
      RecacheMapShapes();
      break;
    }

  case Message::MapShapeReaded:
    {
      ref_ptr<MapShapeReadedMessage> msg = message;
      auto const & tileKey = msg->GetKey();
      if (m_requestedTiles->CheckTileKey(tileKey) && m_readManager->CheckTileKey(tileKey))
      {
        ref_ptr<dp::Batcher> batcher = m_batchersPool->GetBatcher(tileKey);
#if defined(DRAPE_MEASURER) && defined(GENERATING_STATISTIC)
        DrapeMeasurer::Instance().StartShapesGeneration();
#endif
        for (drape_ptr<MapShape> const & shape : msg->GetShapes())
        {
          batcher->SetFeatureMinZoom(shape->GetFeatureMinZoom());
          shape->Draw(batcher, m_texMng);
        }
#if defined(DRAPE_MEASURER) && defined(GENERATING_STATISTIC)
        DrapeMeasurer::Instance().EndShapesGeneration(static_cast<uint32_t>(msg->GetShapes().size()));
#endif
      }
      break;
    }

  case Message::OverlayMapShapeReaded:
    {
      ref_ptr<OverlayMapShapeReadedMessage> msg = message;
      auto const & tileKey = msg->GetKey();
      if (m_requestedTiles->CheckTileKey(tileKey) && m_readManager->CheckTileKey(tileKey))
      {
        CleanupOverlays(tileKey);

#if defined(DRAPE_MEASURER) && defined(GENERATING_STATISTIC)
        DrapeMeasurer::Instance().StartOverlayShapesGeneration();
#endif
        OverlayBatcher batcher(tileKey);
        for (drape_ptr<MapShape> const & shape : msg->GetShapes())
          batcher.Batch(shape, m_texMng);

        TOverlaysRenderData renderData;
        batcher.Finish(renderData);
        if (!renderData.empty())
        {
          m_overlays.reserve(m_overlays.size() + renderData.size());
          move(renderData.begin(), renderData.end(), back_inserter(m_overlays));
        }

#if defined(DRAPE_MEASURER) && defined(GENERATING_STATISTIC)
        DrapeMeasurer::Instance().EndOverlayShapesGeneration(
              static_cast<uint32_t>(msg->GetShapes().size()));
#endif
      }
      break;
    }

  case Message::UpdateUserMarkLayer:
    {
      ref_ptr<UpdateUserMarkLayerMessage> msg = message;

      UserMarksProvider const * marksProvider = msg->StartProcess();
      if (marksProvider->IsDirty())
      {
        size_t const layerId = msg->GetLayerId();
        m_commutator->PostMessage(ThreadsCommutator::RenderThread,
                                  make_unique_dp<ClearUserMarkLayerMessage>(layerId),
                                  MessagePriority::Normal);

        TUserMarkShapes shapes = CacheUserMarks(marksProvider, m_texMng);
        m_commutator->PostMessage(ThreadsCommutator::RenderThread,
                                  make_unique_dp<FlushUserMarksMessage>(layerId, move(shapes)),
                                  MessagePriority::Normal);
      }
      msg->EndProcess();
      break;
    }

  case Message::AddRoute:
    {
      ref_ptr<AddRouteMessage> msg = message;
      m_routeBuilder->Build(msg->GetRoutePolyline(), msg->GetTurns(), msg->GetColor(),
                            msg->GetTraffic(), msg->GetPattern(), m_texMng, msg->GetRecacheId());
      break;
    }

  case Message::CacheRouteSign:
    {
      ref_ptr<CacheRouteSignMessage> msg = message;
      m_routeBuilder->BuildSign(msg->GetPosition(), msg->IsStart(), msg->IsValid(), m_texMng, msg->GetRecacheId());
      break;
    }

  case Message::CacheRouteArrows:
    {
      ref_ptr<CacheRouteArrowsMessage> msg = message;
      m_routeBuilder->BuildArrows(msg->GetRouteIndex(), msg->GetBorders(), m_texMng, msg->GetRecacheId());
      break;
    }

  case Message::RemoveRoute:
    {
      ref_ptr<RemoveRouteMessage> msg = message;
      m_routeBuilder->ClearRouteCache();
      // We have to resend the message to FR, because it guaranties that
      // RemoveRouteMessage will be processed after FlushRouteMessage.
      m_commutator->PostMessage(ThreadsCommutator::RenderThread,
                                make_unique_dp<RemoveRouteMessage>(msg->NeedDeactivateFollowing()),
                                MessagePriority::Normal);
      break;
    }

  case Message::InvalidateTextures:
    {
      m_texMng->Invalidate(VisualParams::Instance().GetResourcePostfix());
      RecacheMapShapes();
      m_trafficGenerator->InvalidateTexturesCache();
      break;
    }

  case Message::CacheGpsTrackPoints:
    {
      ref_ptr<CacheGpsTrackPointsMessage> msg = message;
      drape_ptr<GpsTrackRenderData> data = make_unique_dp<GpsTrackRenderData>();
      data->m_pointsCount = msg->GetPointsCount();
      GpsTrackShape::Draw(m_texMng, *data.get());
      m_commutator->PostMessage(ThreadsCommutator::RenderThread,
                                make_unique_dp<FlushGpsTrackPointsMessage>(move(data)),
                                MessagePriority::Normal);
      break;
    }

  case Message::Allow3dBuildings:
    {
      ref_ptr<Allow3dBuildingsMessage> msg = message;
      m_readManager->Allow3dBuildings(msg->Allow3dBuildings());
      break;
    }

  case Message::RequestSymbolsSize:
    {
      ref_ptr<RequestSymbolsSizeMessage> msg = message;
      auto const & symbols = msg->GetSymbols();

      vector<m2::PointF> sizes(symbols.size());
      for (size_t i = 0; i < symbols.size(); i++)
      {
        dp::TextureManager::SymbolRegion region;
        m_texMng->GetSymbolRegion(symbols[i], region);
        sizes[i] = region.GetPixelSize();
      }
      msg->InvokeCallback(sizes);

      break;
    }

  case Message::EnableTraffic:
    {
      ref_ptr<EnableTrafficMessage> msg = message;
      if (!msg->IsTrafficEnabled())
        m_trafficGenerator->ClearCache();
      m_readManager->SetTrafficEnabled(msg->IsTrafficEnabled());
      m_commutator->PostMessage(ThreadsCommutator::RenderThread,
                                make_unique_dp<EnableTrafficMessage>(msg->IsTrafficEnabled()),
                                MessagePriority::Normal);
      break;
    }

  case Message::FlushTrafficGeometry:
    {
      ref_ptr<FlushTrafficGeometryMessage> msg = message;
      auto const & tileKey = msg->GetKey();
      if (m_requestedTiles->CheckTileKey(tileKey) && m_readManager->CheckTileKey(tileKey))
        m_trafficGenerator->FlushSegmentsGeometry(tileKey, msg->GetSegments(), m_texMng);
      break;
    }

  case Message::UpdateTraffic:
    {
      ref_ptr<UpdateTrafficMessage> msg = message;
      m_trafficGenerator->UpdateColoring(msg->GetSegmentsColoring());
      m_commutator->PostMessage(ThreadsCommutator::RenderThread,
                                make_unique_dp<RegenerateTrafficMessage>(),
                                MessagePriority::Normal);
      break;
    }

  case Message::ClearTrafficData:
    {
      ref_ptr<ClearTrafficDataMessage> msg = message;

      m_trafficGenerator->ClearCache(msg->GetMwmId());

      m_commutator->PostMessage(ThreadsCommutator::RenderThread,
                                make_unique_dp<ClearTrafficDataMessage>(msg->GetMwmId()),
                                MessagePriority::Normal);
      break;
    }

  case Message::SetSimplifiedTrafficColors:
    {
      ref_ptr<SetSimplifiedTrafficColorsMessage> msg = message;

      m_trafficGenerator->SetSimplifiedColorSchemeEnabled(msg->IsSimplified());
      m_trafficGenerator->InvalidateTexturesCache();

      m_commutator->PostMessage(ThreadsCommutator::RenderThread,
                                make_unique_dp<SetSimplifiedTrafficColorsMessage>(msg->IsSimplified()),
                                MessagePriority::Normal);
      break;
    }

  case Message::DrapeApiAddLines:
    {
      ref_ptr<DrapeApiAddLinesMessage> msg = message;
      vector<drape_ptr<DrapeApiRenderProperty>> properties;
      m_drapeApiBuilder->BuildLines(msg->GetLines(), m_texMng, properties);
      m_commutator->PostMessage(ThreadsCommutator::RenderThread,
                                make_unique_dp<DrapeApiFlushMessage>(move(properties)),
                                MessagePriority::Normal);
      break;
    }

  case Message::DrapeApiRemove:
    {
      ref_ptr<DrapeApiRemoveMessage> msg = message;
      m_commutator->PostMessage(ThreadsCommutator::RenderThread,
                                make_unique_dp<DrapeApiRemoveMessage>(msg->GetId(), msg->NeedRemoveAll()),
                                MessagePriority::Normal);
      break;
    }

  case Message::SetCustomSymbols:
    {
      ref_ptr<SetCustomSymbolsMessage> msg = message;
      CustomSymbols customSymbols = msg->AcceptSymbols();
      std::vector<FeatureID> features;
      for (auto const & symbol : customSymbols)
        features.push_back(symbol.first);
      m_readManager->UpdateCustomSymbols(std::move(customSymbols));
      m_commutator->PostMessage(ThreadsCommutator::RenderThread,
                                make_unique_dp<UpdateCustomSymbolsMessage>(std::move(features)),
                                MessagePriority::Normal);
      break;
    }

  default:
    ASSERT(false, ());
    break;
  }
}

void BackendRenderer::ReleaseResources()
{
  m_readManager->Stop();

  m_readManager.reset();
  m_batchersPool.reset();
  m_routeBuilder.reset();
  m_overlays.clear();
  m_trafficGenerator.reset();

  m_texMng->Release();
  m_contextFactory->getResourcesUploadContext()->doneCurrent();
}

void BackendRenderer::OnContextCreate()
{
  LOG(LINFO, ("On context create."));
  m_contextFactory->waitForInitialization();
  m_contextFactory->getResourcesUploadContext()->makeCurrent();

  GLFunctions::Init();

  InitGLDependentResource();
}

void BackendRenderer::OnContextDestroy()
{
  LOG(LINFO, ("On context destroy."));
  m_readManager->InvalidateAll();
  m_batchersPool.reset();
  m_texMng->Release();
  m_overlays.clear();
  m_trafficGenerator->ClearGLDependentResources();

  m_contextFactory->getResourcesUploadContext()->doneCurrent();
}

BackendRenderer::Routine::Routine(BackendRenderer & renderer) : m_renderer(renderer) {}

void BackendRenderer::Routine::Do()
{
  LOG(LINFO, ("Start routine."));
  m_renderer.OnContextCreate();

  while (!IsCancelled())
  {
    m_renderer.ProcessSingleMessage();
    m_renderer.CheckRenderingEnabled();
  }

  m_renderer.ReleaseResources();
}

void BackendRenderer::InitGLDependentResource()
{
  uint32_t constexpr kBatchSize = 5000;
  m_batchersPool = make_unique_dp<BatchersPool<TileKey, TileKeyStrictComparator>>(ReadManager::ReadCount(),
                                               bind(&BackendRenderer::FlushGeometry, this, _1, _2, _3),
                                               kBatchSize, kBatchSize);
  m_trafficGenerator->Init();

  dp::TextureManager::Params params;
  params.m_resPostfix = VisualParams::Instance().GetResourcePostfix();
  params.m_visualScale = df::VisualParams::Instance().GetVisualScale();
  params.m_colors = "colors.txt";
  params.m_patterns = "patterns.txt";
  params.m_glyphMngParams.m_uniBlocks = "unicode_blocks.txt";
  params.m_glyphMngParams.m_whitelist = "fonts_whitelist.txt";
  params.m_glyphMngParams.m_blacklist = "fonts_blacklist.txt";
  params.m_glyphMngParams.m_sdfScale = VisualParams::Instance().GetGlyphSdfScale();
  params.m_glyphMngParams.m_baseGlyphHeight = VisualParams::Instance().GetGlyphBaseSize();
  GetPlatform().GetFontNames(params.m_glyphMngParams.m_fonts);

  m_texMng->Init(params);
}

void BackendRenderer::RecacheMapShapes()
{
  auto msg = make_unique_dp<MapShapesMessage>(make_unique_dp<MyPosition>(m_texMng),
                                              make_unique_dp<SelectionShape>(m_texMng));

  GLFunctions::glFlush();
  m_commutator->PostMessage(ThreadsCommutator::RenderThread, move(msg), MessagePriority::High);
}

void BackendRenderer::FlushGeometry(TileKey const & key, dp::GLState const & state, drape_ptr<dp::RenderBucket> && buffer)
{
  GLFunctions::glFlush();
  m_commutator->PostMessage(ThreadsCommutator::RenderThread,
                            make_unique_dp<FlushRenderBucketMessage>(key, state, move(buffer)),
                            MessagePriority::Normal);
}

void BackendRenderer::FlushTrafficRenderData(TrafficRenderData && renderData)
{
  m_commutator->PostMessage(ThreadsCommutator::RenderThread,
                            make_unique_dp<FlushTrafficDataMessage>(move(renderData)),
                            MessagePriority::Normal);
}

void BackendRenderer::CleanupOverlays(TileKey const & tileKey)
{
  auto const functor = [&tileKey](OverlayRenderData const & data)
  {
    return data.m_tileKey == tileKey && data.m_tileKey.m_generation < tileKey.m_generation;
  };
  m_overlays.erase(remove_if(m_overlays.begin(), m_overlays.end(), functor), m_overlays.end());
}

} // namespace df
