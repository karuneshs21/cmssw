#include "MeasurementTrackerEventProducer.h"

#include "FWCore/Framework/interface/MakerMacros.h"
#include "FWCore/Framework/interface/ESHandle.h"

#include "RecoTracker/MeasurementDet/interface/MeasurementTracker.h"
#include "RecoTracker/MeasurementDet/interface/MeasurementTrackerEvent.h"
#include "RecoTracker/Record/interface/CkfComponentsRecord.h"
#include "CondFormats/SiPixelObjects/interface/CablingPathToDetUnit.h"
#include "CondFormats/SiPixelObjects/interface/PixelROC.h"
#include "CondFormats/SiPixelObjects/interface/LocalPixel.h"

#include <algorithm>

MeasurementTrackerEventProducer::MeasurementTrackerEventProducer(const edm::ParameterSet &iConfig) :
    measurementTrackerLabel_(iConfig.getParameter<std::string>("measurementTracker")),
    pset_(iConfig)
{
    std::vector<edm::InputTag> inactivePixelDetectorTags(iConfig.getParameter<std::vector<edm::InputTag> >("inactivePixelDetectorLabels"));
    for (auto &t : inactivePixelDetectorTags) theInactivePixelDetectorLabels.push_back(consumes<DetIdCollection>(t));

    if ( iConfig.exists("badPixelFEDChannelCollectionLabels") ) {
      std::vector<edm::InputTag> badPixelFEDChannelCollectionTags(iConfig.getParameter<std::vector<edm::InputTag> >("badPixelFEDChannelCollectionLabels"));
      for (auto &t : badPixelFEDChannelCollectionTags) theBadPixelFEDChannelsLabels.push_back(consumes<PixelFEDChannelCollection>(t));
      pixelCablingMapLabel_ = iConfig.getParameter<std::string> ("pixelCablingMapLabel");
    }

    std::vector<edm::InputTag> inactiveStripDetectorTags(iConfig.getParameter<std::vector<edm::InputTag> >("inactiveStripDetectorLabels"));
    for (auto &t : inactiveStripDetectorTags) theInactiveStripDetectorLabels.push_back(consumes<DetIdCollection>(t));

    //the measurement tracking is set to skip clusters, the other option is set from outside
    selfUpdateSkipClusters_=iConfig.exists("skipClusters");
    if (selfUpdateSkipClusters_)
    {
        edm::InputTag skip=iConfig.getParameter<edm::InputTag>("skipClusters");
        if (skip==edm::InputTag("")) selfUpdateSkipClusters_=false;
    }
    LogDebug("MeasurementTracker")<<"skipping clusters: "<<selfUpdateSkipClusters_;
    isPhase2 = false;

    if (pset_.getParameter<std::string>("stripClusterProducer") != "") {
        theStripClusterLabel = consumes<edmNew::DetSetVector<SiStripCluster> >(edm::InputTag(pset_.getParameter<std::string>("stripClusterProducer")));
        if (selfUpdateSkipClusters_) theStripClusterMask = consumes<edm::ContainerMask<edmNew::DetSetVector<SiStripCluster>>>(iConfig.getParameter<edm::InputTag>("skipClusters"));
    }
    if (pset_.getParameter<std::string>("pixelClusterProducer") != "") {
        thePixelClusterLabel = consumes<edmNew::DetSetVector<SiPixelCluster> >(edm::InputTag(pset_.getParameter<std::string>("pixelClusterProducer")));
        if (selfUpdateSkipClusters_) thePixelClusterMask = consumes<edm::ContainerMask<edmNew::DetSetVector<SiPixelCluster>>>(iConfig.getParameter<edm::InputTag>("skipClusters"));
    }
    if (pset_.existsAs<std::string>("Phase2TrackerCluster1DProducer")) {
        thePh2OTClusterLabel = consumes<edmNew::DetSetVector<Phase2TrackerCluster1D> >(edm::InputTag(pset_.getParameter<std::string>("Phase2TrackerCluster1DProducer")));
        isPhase2 = true;
    }

    produces<MeasurementTrackerEvent>();
}

void
MeasurementTrackerEventProducer::produce(edm::Event &iEvent, const edm::EventSetup& iSetup)
{
    edm::ESHandle<MeasurementTracker> measurementTracker;
    iSetup.get<CkfComponentsRecord>().get(measurementTrackerLabel_, measurementTracker);

    edm::ESHandle<SiPixelFedCablingMap> cablingMap;
    iSetup.get<SiPixelFedCablingMapRcd>().get(pixelCablingMapLabel_, cablingMap);

    // create new data structures from templates
    auto stripData = std::make_unique<StMeasurementDetSet>(measurementTracker->stripDetConditions());
    auto pixelData=  std::make_unique<PxMeasurementDetSet>(measurementTracker->pixelDetConditions());
    auto phase2OTData = std::make_unique<Phase2OTMeasurementDetSet>(measurementTracker->phase2DetConditions());
    std::vector<bool> stripClustersToSkip;
    std::vector<bool> pixelClustersToSkip;
    std::vector<bool> phase2ClustersToSkip;
    // fill them
    updateStrips(iEvent, *stripData, stripClustersToSkip);
    updatePixels(iEvent, *pixelData, pixelClustersToSkip, dynamic_cast<const TrackerGeometry&>(*(measurementTracker->geomTracker())), *cablingMap);
    updatePhase2OT(iEvent, *phase2OTData);
    updateStacks(iEvent, *phase2OTData);

    // put into MTE
    // put into event
    iEvent.put(std::move(
      std::make_unique<MeasurementTrackerEvent>(*measurementTracker, 
                                                stripData.release(), pixelData.release(), phase2OTData.release(),
	                                        stripClustersToSkip, pixelClustersToSkip, phase2ClustersToSkip)
    ));
}

void 
MeasurementTrackerEventProducer::updatePixels( const edm::Event& event, PxMeasurementDetSet & thePxDets, std::vector<bool> & pixelClustersToSkip, 
					       const TrackerGeometry& trackerGeom, const SiPixelFedCablingMap& cablingMap) const
{
  // start by clearinng everything
  thePxDets.setEmpty();

  bool switchOffPixelsIfEmpty = pset_.getParameter<bool>("switchOffPixelsIfEmpty");
  std::vector<uint32_t> rawInactiveDetIds; 
  if (!theInactivePixelDetectorLabels.empty()) {
    edm::Handle<DetIdCollection> detIds;
    for (const edm::EDGetTokenT<DetIdCollection> &tk : theInactivePixelDetectorLabels) {
      if (event.getByToken(tk, detIds)){
        rawInactiveDetIds.insert(rawInactiveDetIds.end(), detIds->begin(), detIds->end());
      }else{
        static std::atomic<bool> iFailedAlready{false};
        bool expected = false;
        if (iFailedAlready.compare_exchange_strong(expected,true,std::memory_order_acq_rel)){
          edm::LogError("MissingProduct")<<"I fail to get the list of inactive pixel modules, because of 4.2/4.4 event content change.";
        }
      }
    }
    if (!rawInactiveDetIds.empty()) std::sort(rawInactiveDetIds.begin(), rawInactiveDetIds.end());
    // mark as inactive if in rawInactiveDetIds
    int i=0, endDet = thePxDets.size();
    unsigned int idp=0;
    for ( auto id : rawInactiveDetIds) {
        if (id==idp) continue; // skip multiple id
        idp=id;
        i=thePxDets.find(id,i);
        assert(i!=endDet && id == thePxDets.id(i));
        thePxDets.setActiveThisEvent(i,false);
    }
  }

  if (!theBadPixelFEDChannelsLabels.empty()) {
    edm::Handle<PixelFEDChannelCollection> pixelFEDChannelCollectionHandle;
    for (const edm::EDGetTokenT<PixelFEDChannelCollection>& tk: theBadPixelFEDChannelsLabels) {
      if (!event.getByToken(tk, pixelFEDChannelCollectionHandle)) continue;
      int i=0;
      for (const auto& disabledChannels: *pixelFEDChannelCollectionHandle) {	
	PxMeasurementDetSet::BadFEDChannelPositions positions;
	for(const auto& ch: disabledChannels) {
	  const sipixelobjects::PixelROC *roc_first=NULL, *roc_last=NULL;
	  sipixelobjects::CablingPathToDetUnit path = {ch.fed, ch.link, 0};
	  for (path.roc=1; path.roc<=(ch.roc_last-ch.roc_first)+1; path.roc++) {
	    const sipixelobjects::PixelROC *roc = cablingMap.findItem(path);
	    if (roc==NULL) continue;
	    assert(roc->rawId()==disabledChannels.detId());
	    if (roc->idInDetUnit()==ch.roc_first) roc_first=roc;
	    if (roc->idInDetUnit()==ch.roc_last) roc_last=roc;
	  }
	  if (roc_first==NULL || roc_last==NULL) { 
	    edm::LogError("PixelFEDChannelCollection")<<"Do not find either roc_first or roc_last in the cabling map."; 
	    continue; 
	  }
	  const PixelGeomDetUnit * theGeomDet = dynamic_cast<const PixelGeomDetUnit*> (trackerGeom.idToDet(roc_first->rawId()));
	  PixelTopology const * topology = &(theGeomDet->specificTopology());
	  sipixelobjects::LocalPixel::RocRowCol local = {39, 25};   //corresponding to center of ROC row, col
	  sipixelobjects::GlobalPixel global = roc_first->toGlobal(sipixelobjects::LocalPixel(local));
	  LocalPoint lp1 = topology->localPosition(MeasurementPoint(global.row, global.col));
	  global = roc_last->toGlobal(sipixelobjects::LocalPixel(local));
	  LocalPoint lp2 = topology->localPosition(MeasurementPoint(global.row, global.col));
	  LocalPoint ll(std::min(lp1.x(), lp2.x()), std::min(lp1.y(), lp2.y()), std::min(lp1.z(), lp2.z()));
	  LocalPoint ur(std::max(lp1.x(), lp2.x()), std::max(lp1.y(), lp2.y()), std::max(lp1.z(), lp2.z()));	  
	  positions.push_back(std::make_pair(ll, ur));
	} // loop on channels
	if (positions.size()) {
	  i=thePxDets.find(disabledChannels.detId(),i);
	  assert(i!=thePxDets.size() && thePxDets.id(i)==disabledChannels.detId());
	  thePxDets.addBadFEDChannelPositions(i, positions);
	}
      } // loop on DetId-s
    } // loop on labels
  } // if collection labels are populated

  // Pixel Clusters
  std::string pixelClusterProducer = pset_.getParameter<std::string>("pixelClusterProducer");
  if( pixelClusterProducer.empty() ) { //clusters have not been produced
    if (switchOffPixelsIfEmpty) {
      thePxDets.setActiveThisEvent(false);
    }
  }else{  

    edm::Handle<edmNew::DetSetVector<SiPixelCluster> > & pixelClusters = thePxDets.handle();
    if(event.getByToken(thePixelClusterLabel, pixelClusters)) {
    
      const  edmNew::DetSetVector<SiPixelCluster>* pixelCollection = pixelClusters.product();
      
   
      if (switchOffPixelsIfEmpty && pixelCollection->empty()) {
	thePxDets.setActiveThisEvent(false);
      } else { 
	
	//std::cout <<"updatePixels "<<pixelCollection->dataSize()<<std::endl;
	pixelClustersToSkip.resize(pixelCollection->dataSize());
	std::fill(pixelClustersToSkip.begin(),pixelClustersToSkip.end(),false);
	
	if(selfUpdateSkipClusters_) {
          edm::Handle<edm::ContainerMask<edmNew::DetSetVector<SiPixelCluster> > > pixelClusterMask;
	  //and get the collection of pixel ref to skip
	  event.getByToken(thePixelClusterMask,pixelClusterMask);
	  LogDebug("MeasurementTracker")<<"getting pxl refs to skip";
	  if (pixelClusterMask.failedToGet())edm::LogError("MeasurementTracker")<<"not getting the pixel clusters to skip";
	  if (pixelClusterMask->refProd().id()!=pixelClusters.id()){
	    edm::LogError("ProductIdMismatch")<<"The pixel masking does not point to the proper collection of clusters: "<<pixelClusterMask->refProd().id()<<"!="<<pixelClusters.id();
	  }
	  pixelClusterMask->copyMaskTo(pixelClustersToSkip);
	}
	
	
	// FIXME: should check if lower_bound is better
	int i = 0, endDet = thePxDets.size();
	for (edmNew::DetSetVector<SiPixelCluster>::const_iterator it = pixelCollection->begin(), ed = pixelCollection->end(); it != ed; ++it) {
	  edmNew::DetSet<SiPixelCluster> set(*it);
	  unsigned int id = set.id();
	  while ( id != thePxDets.id(i)) { 
	    ++i;
	    if (endDet==i) throw "we have a problem!!!!";
	  }
	  // push cluster range in det
	  if ( thePxDets.isActive(i) ) {
	    thePxDets.update(i,set);         
	  }
	}
      }
    } else {
      edm::LogWarning("MeasurementTrackerEventProducer") << "input pixel clusters collection " << pset_.getParameter<std::string>("pixelClusterProducer") << " is not valid";
    }
  }
}

void 
MeasurementTrackerEventProducer::updateStrips( const edm::Event& event, StMeasurementDetSet & theStDets, std::vector<bool> & stripClustersToSkip ) const
{
  typedef edmNew::DetSet<SiStripCluster>   StripDetSet;

  std::vector<uint32_t> rawInactiveDetIds;
  getInactiveStrips(event,rawInactiveDetIds);

  // Strip Clusters
  std::string stripClusterProducer = pset_.getParameter<std::string>("stripClusterProducer");
  //first clear all of them
  theStDets.setEmpty();


  if( !stripClusterProducer.compare("") )  return;  //clusters have not been produced

  const int endDet = theStDets.size();
 

  // mark as inactive if in rawInactiveDetIds
  int i=0;
  unsigned int idp=0;
  for ( auto id : rawInactiveDetIds) {
    if (id==idp) continue; // skip multiple id
    idp=id;
    i=theStDets.find(id,i);
    assert(i!=endDet && id == theStDets.id(i));
    theStDets.setActiveThisEvent(i,false);
  }

  //=========  actually load cluster =============
  {
    edm::Handle<edmNew::DetSetVector<SiStripCluster> > clusterHandle;
    if (event.getByToken(theStripClusterLabel, clusterHandle)) {
      const edmNew::DetSetVector<SiStripCluster>* clusterCollection = clusterHandle.product();
    
    
      if (selfUpdateSkipClusters_){
	edm::Handle<edm::ContainerMask<edmNew::DetSetVector<SiStripCluster> > > stripClusterMask;
	//and get the collection of pixel ref to skip
	LogDebug("MeasurementTracker")<<"getting strp refs to skip";
	event.getByToken(theStripClusterMask,stripClusterMask);
	if (stripClusterMask.failedToGet())  edm::LogError("MeasurementTracker")<<"not getting the strip clusters to skip";
	if (stripClusterMask->refProd().id()!=clusterHandle.id()){
	  edm::LogError("ProductIdMismatch")<<"The strip masking does not point to the proper collection of clusters: "<<stripClusterMask->refProd().id()<<"!="<<clusterHandle.id();
	}
	stripClusterMask->copyMaskTo(stripClustersToSkip);
      }
    
      theStDets.handle() = clusterHandle;
      int i=0;
      // cluster and det and in order (both) and unique so let's use set intersection
      for ( auto j = 0U; j< (*clusterCollection).size(); ++j) {
	unsigned int id = (*clusterCollection).id(j);
	while ( id != theStDets.id(i)) { // eventually change to lower_bound
	  ++i;
	  if (endDet==i) throw "we have a problem in strips!!!!";
	}
      
	// push cluster range in det
	if ( theStDets.isActive(i) )
	  theStDets.update(i,j);
      }
    } else {
      edm::LogWarning("MeasurementTrackerEventProducer") << "input strip cluster collection " << pset_.getParameter<std::string>("stripClusterProducer") << " is not valid";
    }
  }
}

//FIXME: just a temporary solution for phase2!
void 
MeasurementTrackerEventProducer::updatePhase2OT( const edm::Event& event, Phase2OTMeasurementDetSet & thePh2OTDets ) const {


  // Phase2OT Clusters
  if ( isPhase2 ) {

    std::string phase2ClusterProducer = pset_.getParameter<std::string>("Phase2TrackerCluster1DProducer");
    if( phase2ClusterProducer.empty() ) { //clusters have not been produced
      thePh2OTDets.setActiveThisEvent(false);
    } else {
  
      edm::Handle<edmNew::DetSetVector<Phase2TrackerCluster1D> > & phase2OTClusters = thePh2OTDets.handle();
      if (event.getByToken(thePh2OTClusterLabel, phase2OTClusters)) {
	const edmNew::DetSetVector<Phase2TrackerCluster1D>* phase2OTCollection = phase2OTClusters.product();
  
	int i = 0, endDet = thePh2OTDets.size();
	for (edmNew::DetSetVector<Phase2TrackerCluster1D>::const_iterator it = phase2OTCollection->begin(), ed = phase2OTCollection->end(); it != ed; ++it) {
	  
	  edmNew::DetSet<Phase2TrackerCluster1D> set(*it);
	  unsigned int id = set.id();
	  while ( id != thePh2OTDets.id(i)) {
            ++i;
            if (endDet==i) throw "we have a problem!!!!";
	  }
	  // push cluster range in det
	  if ( thePh2OTDets.isActive(i) ) {
            thePh2OTDets.update(i,set);
	  }
	}
      } else {
	edm::LogWarning("MeasurementTrackerEventProducer") << "input Phase2TrackerCluster1D collection " << pset_.getParameter<std::string>("Phase2TrackerCluster1DProducer") << " is not valid";
      }
    }

  }
  return;
}

void
MeasurementTrackerEventProducer::getInactiveStrips(const edm::Event& event,std::vector<uint32_t> & rawInactiveDetIds) const
{
  if (!theInactiveStripDetectorLabels.empty()) {
    edm::Handle<DetIdCollection> detIds;
    for (const edm::EDGetTokenT<DetIdCollection> &tk : theInactiveStripDetectorLabels) {
        if (event.getByToken(tk, detIds)){
            rawInactiveDetIds.insert(rawInactiveDetIds.end(), detIds->begin(), detIds->end());
        }
    }
    if (!rawInactiveDetIds.empty()) std::sort(rawInactiveDetIds.begin(), rawInactiveDetIds.end());
  }

}



DEFINE_FWK_MODULE(MeasurementTrackerEventProducer);
