/*! \file PeridigmManager.cpp */

#include "PeridigmManager.hpp"
#include "Peridigm_ProximitySearch.hpp"
#include "Albany_Utils.hpp"
#include <stk_mesh/base/GetEntities.hpp>
#include <stk_mesh/base/FieldBase.hpp>
#include "Phalanx_DataLayout.hpp"
#include "QCAD_MaterialDatabase.hpp"
#include "PHAL_Dimension.hpp"

LCM::PeridigmManager& LCM::PeridigmManager::self() {
  static PeridigmManager peridigmManager;
  return peridigmManager;
}

LCM::PeridigmManager::PeridigmManager() : hasPeridynamics(false), previousTime(0.0), currentTime(0.0), timeStep(0.0), cubatureDegree(-1)
{}

void LCM::PeridigmManager::initialize(const Teuchos::RCP<Teuchos::ParameterList>& params,
                                      Teuchos::RCP<Albany::AbstractDiscretization> disc,
				      const Teuchos::RCP<const Teuchos_Comm>& comm)
{
  if(!params->sublist("Problem").isSublist("Peridigm Parameters")){
    hasPeridynamics = false;
    return;
  }

  teuchosComm = comm;
  peridigmParams = Teuchos::RCP<Teuchos::ParameterList>(new Teuchos::ParameterList(params->sublist("Problem").sublist("Peridigm Parameters", true)));
  Teuchos::ParameterList& problemParams = params->sublist("Problem");
  Teuchos::ParameterList& discretizationParams = params->sublist("Discretization");
  cubatureDegree = discretizationParams.get<int>("Cubature Degree", 2);

  // Read the material data base file, if any
  Teuchos::RCP<QCAD::MaterialDatabase> materialDataBase;
  if(problemParams.isType<std::string>("MaterialDB Filename")){
    std::string filename = problemParams.get<std::string>("MaterialDB Filename");
    materialDataBase = Teuchos::rcp(new QCAD::MaterialDatabase(filename, teuchosComm));
  }

  Teuchos::RCP<Albany::STKDiscretization> stkDisc = Teuchos::rcp_dynamic_cast<Albany::STKDiscretization>(disc);
  TEUCHOS_TEST_FOR_EXCEPT_MSG(stkDisc.is_null(), "\n\n**** Error in PeridigmManager::initialize():  Peridigm interface is valid only for STK meshes.\n\n");
  metaData = Teuchos::rcpFromRef(stkDisc->getSTKMetaData());
  bulkData = Teuchos::rcpFromRef(stkDisc->getSTKBulkData());
  TEUCHOS_TEST_FOR_EXCEPT_MSG(metaData->spatial_dimension() != 3, "\n\n**** Error in PeridigmManager::initialize():  Peridigm interface is valid only for three-dimensional meshes.\n\n");

  // Store the cell topology for each element mesh part
  std::map<std::string,CellTopologyData> partCellTopologyData; 

  const stk::mesh::PartVector& stkParts = metaData->get_parts();
  stk::mesh::PartVector stkElementBlocks;
  for(stk::mesh::PartVector::const_iterator it = stkParts.begin(); it != stkParts.end(); ++it){
    stk::mesh::Part* const part = *it;
    if(!stk::mesh::is_auto_declared_part(*part) && 
       part->primary_entity_rank() == stk::topology::ELEMENT_RANK){
      stkElementBlocks.push_back(part);
      partCellTopologyData[part->name()] = *metaData->get_cell_topology(*part).getCellTopologyData();
    }
  }

//   const stk::mesh::FieldVector &fields = metaData->get_fields();
//   for(unsigned int i=0 ; i<fields.size() ; ++i)
//     std::cout << "DJL DEBUGGING STK field " << *fields[i] << std::endl;

  stk::mesh::Field<double,stk::mesh::Cartesian3d>* coordinatesField = 
    metaData->get_field< stk::mesh::Field<double,stk::mesh::Cartesian3d> >(stk::topology::NODE_RANK, "coordinates");
  TEUCHOS_TEST_FOR_EXCEPT_MSG(coordinatesField == 0, "\n\n**** Error in PeridigmManager::initialize(), unable to access coordinates field.\n\n");

  stk::mesh::Field<double,stk::mesh::Cartesian3d>* volumeField = 
    metaData->get_field< stk::mesh::Field<double,stk::mesh::Cartesian3d> >(stk::topology::ELEMENT_RANK, "volume");

  // Create a selector to select everything in the universal part that is either locally owned or globally shared
  stk::mesh::Selector selector = 
    stk::mesh::Selector( metaData->universal_part() ) & ( stk::mesh::Selector( metaData->locally_owned_part() ) | stk::mesh::Selector( metaData->globally_shared_part() ) );

  // Select element mesh entities that match the selector
  std::vector<stk::mesh::Entity> elements;
  stk::mesh::get_selected_entities(selector, bulkData->buckets(stk::topology::ELEMENT_RANK), elements);

  // List of blocks for peridynamics, partial stress, and standard FEM
  std::vector<std::string> peridynamicsBlocks, peridynamicPartialStressBlocks, classicalContinuumMechanicsBlocks;

  int numPartialStressIds(0);

  // Bookkeeping so that partial stress nodes on the Peridigm side are guaranteed to have ids that don't exist in the Albany discretization
  int maxAlbanyElementId(0), maxAlbanyNodeId(0);

  // Store the global node id for each sphere element that will be used for "Peridynamics" materials
  // Store necessary information for each Gauss point in a solid element for "Peridynamic Partial Stress" materials
  for(unsigned int iBlock=0 ; iBlock<stkElementBlocks.size() ; iBlock++){

    // Determine the block id under the assumption that the block names follow the format "block_1", "block_2", etc.
    // Older versions of stk did not have the ability to return the block id directly, I think newer versions of stk can do this however
    const std::string blockName = stkElementBlocks[iBlock]->name();
    size_t loc = blockName.find_last_of('_');
    TEUCHOS_TEST_FOR_EXCEPT_MSG(loc == string::npos, "\n**** Parse error in PeridigmManager::initialize(), invalid block name: " + blockName + "\n");
    stringstream blockIDSS(blockName.substr(loc+1, blockName.size()));
    int bId;
    blockIDSS >> bId;
    blockNameToBlockId[blockName] = bId;

    // Create a selector for all locally-owned elements in the block
    stk::mesh::Selector selector = 
      stk::mesh::Selector( *stkElementBlocks[iBlock] ) & stk::mesh::Selector( metaData->locally_owned_part() );

    // Select the mesh entities that match the selector
    std::vector<stk::mesh::Entity> elementsInElementBlock;
    stk::mesh::get_selected_entities(selector, bulkData->buckets(stk::topology::ELEMENT_RANK), elementsInElementBlock);

    // Determine the material model assigned to this block
    std::string materialModelName;
    if(!materialDataBase.is_null())
      materialModelName = materialDataBase->getElementBlockSublist(blockName, "Material Model").get<std::string>("Model Name");

    // Sphere elements with the "Peridynamics" material model
    if(materialModelName == "Peridynamics"){
      peridynamicsBlocks.push_back(blockName);
      for(unsigned int iElement=0 ; iElement<elementsInElementBlock.size() ; iElement++){
	int numNodes = bulkData->num_nodes(elementsInElementBlock[iElement]);
	TEUCHOS_TEST_FOR_EXCEPT_MSG(numNodes != 1,
				    "\n\n**** Error in PeridigmManager::initialize(), \"Peridynamics\" material model may be assigned only to sphere elements.  Multiple nodes per element detected..\n\n");
	const stk::mesh::Entity* nodes = bulkData->begin_nodes(elementsInElementBlock[iElement]);
	int globalId = bulkData->identifier(nodes[0]) - 1;
	int localId = static_cast<int>(peridigmNodeGlobalIds.size());
	peridigmNodeGlobalIds.push_back(globalId);
	peridigmGlobalIdToPeridigmLocalId[globalId] = localId;
      }
    }
    // Standard solid elements with the "Peridynamic Partial Stress" material model
    else if(materialModelName == "Peridynamic Partial Stress"){
      peridynamicPartialStressBlocks.push_back(blockName);
      CellTopologyData& cellTopologyData = partCellTopologyData[blockName];
      shards::CellTopology cellTopology(&cellTopologyData);
      Intrepid::DefaultCubatureFactory<RealType> cubFactory;
      Teuchos::RCP<Intrepid::Cubature<RealType> > cubature = cubFactory.create(cellTopology, cubatureDegree);
      const int numQPts = cubature->getNumPoints();
      numPartialStressIds += numQPts * elementsInElementBlock.size();
    }
    // Standard solid elements with a classical continum mechanics model
    else{
      classicalContinuumMechanicsBlocks.push_back(blockName);
    }

    // Track the max element and node id in the Albany discretization
    for(unsigned int iElement=0 ; iElement<elementsInElementBlock.size() ; iElement++){
      int elementId = bulkData->identifier(elementsInElementBlock[iElement]) - 1;
      if(elementId > maxAlbanyElementId)
	maxAlbanyElementId = elementId;
      int numNodes = bulkData->num_nodes(elementsInElementBlock[iElement]);
      const stk::mesh::Entity* nodes = bulkData->begin_nodes(elementsInElementBlock[iElement]);
      for(int i=0 ; i<numNodes ; ++i){
	int nodeId = bulkData->identifier(nodes[i]) - 1;
	if(nodeId > maxAlbanyNodeId)
	  maxAlbanyNodeId = nodeId;
      }
    }
  }

  // Determine the Peridigm node ids for the Gauss points in the partial stress elements

  int numProc = teuchosComm->getSize();
  int pid = teuchosComm->getRank();

  // Find the minimum global id across all processors
  int lowestPossiblePartialStressId = maxAlbanyElementId + 1;
  if(maxAlbanyNodeId > lowestPossiblePartialStressId)
    lowestPossiblePartialStressId = maxAlbanyNodeId + 1;
  vector<int> localVal(1), globalVal(1);
  localVal[0] = lowestPossiblePartialStressId;
  Teuchos::reduceAll(*teuchosComm, Teuchos::REDUCE_MAX, 1, &localVal[0], &globalVal[0]);
  lowestPossiblePartialStressId = globalVal[0];

  int minPeridigmPartialStressId = lowestPossiblePartialStressId;

  for(int iProc=0 ; iProc<numProc ; iProc++){

    // Let all processors know how many partial stress nodes are on processor iProc
    localVal[0] = 0;
    if(pid == iProc)
      localVal[0] = numPartialStressIds;
    Teuchos::reduceAll(*teuchosComm, Teuchos::REDUCE_MAX, 1, &localVal[0], &globalVal[0]);

    // Adjust the min partial stress id such that processors will not end up with the same global ids
    if(pid > iProc)
      minPeridigmPartialStressId += globalVal[0];
  }

  std::vector<int> peridigmPartialStressLocalIds;
  for(int i=0 ; i<numPartialStressIds ; i++){
    int peridigmGlobalId = minPeridigmPartialStressId + i;
    int localId = static_cast<int>(peridigmNodeGlobalIds.size());
    peridigmNodeGlobalIds.push_back(peridigmGlobalId);
    peridigmPartialStressLocalIds.push_back(localId);
    peridigmGlobalIdToPeridigmLocalId[peridigmGlobalId] = localId;
  }

  // Write block information to stdout
  std::cout << "\n---- PeridigmManager ----";
  std::cout << "\n  peridynamics blocks:";
  for(unsigned int i=0 ; i<peridynamicsBlocks.size() ; ++i)
    std::cout << " " << peridynamicsBlocks[i];
  std::cout << "\n  peridynamic partial stres blocks:";
  for(unsigned int i=0 ; i<peridynamicPartialStressBlocks.size() ; ++i)
    std::cout << " " << peridynamicPartialStressBlocks[i];
  std::cout << "\n  classical continuum mechanics blocks:";
  for(unsigned int i=0 ; i<classicalContinuumMechanicsBlocks.size() ; ++i)
    std::cout << " " << classicalContinuumMechanicsBlocks[i];
  std::cout << "\n  max Albany element id: " << maxAlbanyElementId << std::endl;
  std::cout << "  max Albany node id: " << maxAlbanyNodeId << std::endl;
  std::cout << "  min Peridigm partial stress id: " << minPeridigmPartialStressId << std::endl;
  std::cout << "  number of Peridigm partial stress material points: " << numPartialStressIds << "\n" << std::endl;

  // Bail if there are no sphere elements or partial stress elements
  if(peridynamicsBlocks.size() == 0 && peridynamicPartialStressBlocks.size() == 0){
    hasPeridynamics = false;
    return;
  }
  else{
    hasPeridynamics = true;
  }

  std::vector<double> initialX(3*peridigmNodeGlobalIds.size());
  std::vector<double> cellVolume(peridigmNodeGlobalIds.size());
  std::vector<int> blockId(peridigmNodeGlobalIds.size());

  // loop over the element blocks and store the initial positions, volume, and block_id
  int peridigmPartialStressIndex = 0;
  for(unsigned int iBlock=0 ; iBlock<stkElementBlocks.size() ; iBlock++){

    const std::string blockName = stkElementBlocks[iBlock]->name();
    int bId = blockNameToBlockId[blockName];

    // Create a selector for all locally-owned elements in the block
    stk::mesh::Selector selector = 
      stk::mesh::Selector( *stkElementBlocks[iBlock] ) & stk::mesh::Selector( metaData->locally_owned_part() );

    // Select the mesh entities that match the selector
    std::vector<stk::mesh::Entity> elementsInElementBlock;
    stk::mesh::get_selected_entities(selector, bulkData->buckets(stk::topology::ELEMENT_RANK), elementsInElementBlock);

    // Determine the material model assigned to this block
    std::string materialModelName;
    if(!materialDataBase.is_null())
      materialModelName = materialDataBase->getElementBlockSublist(blockName, "Material Model").get<std::string>("Model Name");

    if(materialModelName == "Peridynamics"){
      TEUCHOS_TEST_FOR_EXCEPT_MSG(volumeField == 0, "\n\n**** Error in PeridigmManager::initialize(), unable to access volume field.\n\n");
      for(unsigned int iElement=0 ; iElement<elementsInElementBlock.size() ; iElement++){
	int numNodes = bulkData->num_nodes(elementsInElementBlock[iElement]);
	TEUCHOS_TEST_FOR_EXCEPT_MSG(numNodes != 1, "\n\n**** Error in PeridigmManager::initialize(), \"Peridynamics\" material model may be assigned only to sphere elements.\n\n");
	const stk::mesh::Entity* node = bulkData->begin_nodes(elementsInElementBlock[iElement]);
	int globalId = bulkData->identifier(node[0]) - 1;
	int localId = peridigmGlobalIdToPeridigmLocalId[globalId];
	TEUCHOS_TEST_FOR_EXCEPT_MSG(localId == -1, "\n\n**** Error in PeridigmManager::initialize(), invalid global id.\n\n");
	blockId[localId] = bId;
	double* exodusVolume = stk::mesh::field_data(*volumeField, elementsInElementBlock[iElement]);
	TEUCHOS_TEST_FOR_EXCEPT_MSG(exodusVolume == 0, "\n\n**** Error in PeridigmManager::initialize(), failed to access element's volume field.\n\n");
	cellVolume[localId] = exodusVolume[0];
	double* exodusCoordinates = stk::mesh::field_data(*coordinatesField, node[0]);
	TEUCHOS_TEST_FOR_EXCEPT_MSG(exodusCoordinates == 0, "\n\n**** Error in PeridigmManager::initialize(), failed to access element's coordinates field.\n\n");
	initialX[localId*3]   = exodusCoordinates[0];
	initialX[localId*3+1] = exodusCoordinates[1];
	initialX[localId*3+2] = exodusCoordinates[2];
	sphereElementGlobalNodeIds.push_back(globalId);
      }
    }

    else if(materialModelName == "Peridynamic Partial Stress"){

      CellTopologyData& cellTopologyData = partCellTopologyData[blockName];
      shards::CellTopology cellTopology(&cellTopologyData);
      Intrepid::DefaultCubatureFactory<RealType> cubFactory;
      Teuchos::RCP<Intrepid::Cubature<RealType> > cubature = cubFactory.create(cellTopology, cubatureDegree);
      const int numDim = cubature->getDimension();
      const int numQuadPoints = cubature->getNumPoints();
      const int numNodes = cellTopology.getNodeCount();
      const int numCells = 1;

      // Get the quadrature points and weights
      Intrepid::FieldContainer<RealType> quadratureRefPoints;
      Intrepid::FieldContainer<RealType> quadratureRefWeights;
      quadratureRefPoints.resize(numQuadPoints, numDim);
      quadratureRefWeights.resize(numQuadPoints);
      cubature->getCubature(quadratureRefPoints, quadratureRefWeights);

      // Container for the Jacobians, Jacobian determinants, and weighted measures
      Intrepid::FieldContainer<RealType> jacobians;
      Intrepid::FieldContainer<RealType> jacobianDeterminants;
      Intrepid::FieldContainer<RealType> weightedMeasures;
      jacobians.resize(numCells, numQuadPoints, numDim, numDim);
      jacobianDeterminants.resize(numCells, numQuadPoints);
      weightedMeasures.resize(numCells, numQuadPoints);

      // Create data structures for passing information to/from Intrepid.

      typedef PHX::KokkosViewFactory<RealType, PHX::Device> ViewFactory;

      // Physical points, which are the physical (x, y, z) values of the quadrature points
      Teuchos::RCP< PHX::MDALayout<Cell, QuadPoint, Dim> > physPointsLayout = Teuchos::rcp(new PHX::MDALayout<Cell, QuadPoint, Dim>(numCells, numQuadPoints, numDim));
      PHX::MDField<RealType, Cell, QuadPoint, Dim> physPoints("Physical Points", physPointsLayout);
      physPoints.setFieldData(ViewFactory::buildView(physPoints.fieldTag()));

      // Reference points, which are the natural coordinates of the quadrature points
      Teuchos::RCP< PHX::MDALayout<Cell, QuadPoint, Dim> > refPointsLayout = Teuchos::rcp(new PHX::MDALayout<Cell, QuadPoint, Dim>(numCells, numQuadPoints, numDim));
      PHX::MDField<RealType, Cell, QuadPoint, Dim> refPoints("Reference Points", refPointsLayout);
      refPoints.setFieldData(ViewFactory::buildView(refPoints.fieldTag()));

      // Cell workset, which is the set of nodes for the given element
      Teuchos::RCP< PHX::MDALayout<Cell, Node, Dim> > cellWorksetLayout = Teuchos::rcp(new PHX::MDALayout<Cell, Node, Dim>(numCells, numNodes, numDim));
      PHX::MDField<RealType, Cell, Node, Dim> cellWorkset("Cell Workset", cellWorksetLayout);
      cellWorkset.setFieldData(ViewFactory::buildView(cellWorkset.fieldTag()));

      // Copy the reference points from the Intrepid::FieldContainer to a PHX::MDField
      for(int qp=0 ; qp<numQuadPoints ; ++qp){
	for(int dof=0 ; dof<3 ; ++dof){
	  refPoints(0, qp, dof) = quadratureRefPoints(qp, dof);
	}
      }

      for(unsigned int iElement=0 ; iElement<elementsInElementBlock.size() ; iElement++){
	int numNodesInElement = bulkData->num_nodes(elementsInElementBlock[iElement]);
	TEUCHOS_TEST_FOR_EXCEPT_MSG(numNodesInElement != numNodes, "\n\n**** Error in PeridigmManager::initialize(), bulkData->num_nodes() != numNodes.\n\n");
	const stk::mesh::Entity* node = bulkData->begin_nodes(elementsInElementBlock[iElement]);
	for(int i=0 ; i<numNodes ; i++){
	  double* coordinates = stk::mesh::field_data(*coordinatesField, node[i]);
	  for(int dof=0 ; dof<3 ; ++dof)
	    cellWorkset(0, i, dof) = coordinates[dof];
	}

	// Determine the global (x,y,z) coordinates of the quadrature points
  	Intrepid::CellTools<RealType>::mapToPhysicalFrame(physPoints, refPoints, cellWorkset, cellTopology);

	// Determine the weighted integration measures, which are the volumes that will be assigned to the peridynamic material points
 	Intrepid::CellTools<RealType>::setJacobian(jacobians, refPoints, cellWorkset, cellTopology);
 	Intrepid::CellTools<RealType>::setJacobianDet(jacobianDeterminants, jacobians);
 	Intrepid::FunctionSpaceTools::computeCellMeasure<RealType>(weightedMeasures, jacobianDeterminants, quadratureRefWeights);

	// Bookkeeping for use downstream
	PartialStressElement partialStressElement;
	partialStressElement.albanyElement = elementsInElementBlock[iElement];
	partialStressElement.cellTopologyData = cellTopologyData;

	for(unsigned int i=0 ; i<numNodes ; ++i){
	  partialStressElement.albanyNodeInitialPositions.push_back( cellWorkset(0, i, 0) );
	  partialStressElement.albanyNodeInitialPositions.push_back( cellWorkset(0, i, 1) );
	  partialStressElement.albanyNodeInitialPositions.push_back( cellWorkset(0, i, 2) );
	}

	for(unsigned int qp=0 ; qp<numQuadPoints ; ++qp){
	  int localId = peridigmPartialStressLocalIds[peridigmPartialStressIndex++];
	  int globalId = peridigmNodeGlobalIds[localId];
	  blockId[localId] = bId;
	  cellVolume[localId] = weightedMeasures(0, qp);
	  initialX[localId*3]   = physPoints(0, qp, 0);
	  initialX[localId*3+1] = physPoints(0, qp, 1);
	  initialX[localId*3+2] = physPoints(0, qp, 2);
	  partialStressElement.peridigmGlobalIds.push_back(globalId);
	}

	partialStressElements.push_back(partialStressElement);
      }
    }
  }

  // Create a vector for storing the previous solution (from last converged load step)
  previousSolutionPositions = std::vector<double>(initialX.size());
  for(unsigned int i=0 ; i<initialX.size() ; ++i)
    previousSolutionPositions[i] = initialX[i];

  // Create a Peridigm discretization
  const Teuchos::MpiComm<int>* mpiComm = dynamic_cast<const Teuchos::MpiComm<int>* >(teuchosComm.get());
  TEUCHOS_TEST_FOR_EXCEPT_MSG(mpiComm == 0, "\n\n**** Error in PeridigmManager::initialize(), failed to dynamically cast comm object to Teuchos::MpiComm<int>.\n");
  peridynamicDiscretization = Teuchos::rcp<PeridigmNS::Discretization>(new PeridigmNS::AlbanyDiscretization(*mpiComm->getRawMpiComm(),
                                                                                                            peridigmParams,
													    static_cast<int>(peridigmNodeGlobalIds.size()),
													    &peridigmNodeGlobalIds[0],
                                                                                                            &initialX[0],
                                                                                                            &cellVolume[0],
                                                                                                            &blockId[0]));

  // Create a Peridigm object
  peridigm = Teuchos::rcp<PeridigmNS::Peridigm>(new PeridigmNS::Peridigm(*mpiComm->getRawMpiComm(),
									 peridigmParams,
									 peridynamicDiscretization));

  // Create data structure for obtaining the global element id given the workset index and workset local element id.
  Albany::WsLIDList wsLIDList = stkDisc->getElemGIDws();
  for(Albany::WsLIDList::iterator it=wsLIDList.begin() ; it!=wsLIDList.end() ; ++it){
    int globalElementId = it->first;
    int worksetIndex = it->second.ws;
    int worksetLocalId = it->second.LID;
    std::vector<int>& wsGIDs = worksetLocalIdToGlobalId[worksetIndex];
    TEUCHOS_TEST_FOR_EXCEPT_MSG(worksetLocalId != wsGIDs.size(), "\n\n**** Error in PeridigmManager::initialize(), unexpected workset local id.\n\n");
    wsGIDs.push_back(globalElementId);
  }

  // Create a data structure for obtaining the Peridigm global ids given the global id of a Albany partial stress element
  for(unsigned int i=0 ; i<partialStressElements.size() ; ++i){
    int albanyGlobalElementId = bulkData->identifier(partialStressElements[i].albanyElement) - 1;
    vector<int>& peridigmGlobalIds = partialStressElements[i].peridigmGlobalIds;
    albanyPartialStressElementGlobalIdToPeridigmGlobalIds[albanyGlobalElementId] = peridigmGlobalIds;
  }

  //overlappingElementSearch();
}

void LCM::PeridigmManager::overlappingElementSearch()
{
  // ---- Determine the largest element dimension in the model ----

  double largestElementDimension = 0.0;

  stk::mesh::Field<double,stk::mesh::Cartesian3d>* coordinatesField = 
    metaData->get_field< stk::mesh::Field<double,stk::mesh::Cartesian3d> >(stk::topology::NODE_RANK, "coordinates");
  TEUCHOS_TEST_FOR_EXCEPT_MSG(coordinatesField == 0, "\n\n**** Error in PeridigmManager::overlappingElementSearch(), unable to access coordinates field.\n\n");

  stk::mesh::Field<double,stk::mesh::Cartesian3d>* volumeField = 
    metaData->get_field< stk::mesh::Field<double,stk::mesh::Cartesian3d> >(stk::topology::ELEMENT_RANK, "volume");

  // Create a selector to select everything in the universal part that is either locally owned or globally shared
  stk::mesh::Selector selector = 
    stk::mesh::Selector( metaData->universal_part() ) & ( stk::mesh::Selector( metaData->locally_owned_part() ) | stk::mesh::Selector( metaData->globally_shared_part() ) );

  // Select element mesh entities that match the selector
  std::vector<stk::mesh::Entity> elements;
  stk::mesh::get_selected_entities(selector, bulkData->buckets(stk::topology::ELEMENT_RANK), elements);

  for(unsigned int iElem=0 ; iElem<elements.size() ; ++iElem){
    int numNodes = bulkData->num_nodes(elements[iElem]);
    // skip sphere elements, consider only solid elements
    if(numNodes > 1){
      const stk::mesh::Entity* nodes = bulkData->begin_nodes(elements[iElem]);
      for(int i=0 ; i<numNodes ; ++i){
	for(int j=i+1 ; j<numNodes ; ++j){
	  double* pt1 = stk::mesh::field_data(*coordinatesField, nodes[i]);
	  double* pt2 = stk::mesh::field_data(*coordinatesField, nodes[j]);
	  double distanceSquared = (pt1[0]-pt2[0])*(pt1[0]-pt2[0]) + (pt1[1]-pt2[1])*(pt1[1]-pt2[1]) + (pt1[2]-pt2[2])*(pt1[2]-pt2[2]);
	  if(distanceSquared > largestElementDimension){
	    largestElementDimension = distanceSquared;
	  }
	}
      }
    }
  }
  largestElementDimension = std::sqrt(largestElementDimension);

  vector<double> localDoubleVal(1), globalDoubleVal(1);
  localDoubleVal[0] = largestElementDimension;
  Teuchos::reduceAll(*teuchosComm, Teuchos::REDUCE_MAX, 1, &localDoubleVal[0], &globalDoubleVal[0]);
  largestElementDimension = globalDoubleVal[0];

  double proximitySearchRadius = 1.1 * largestElementDimension;

  // ---- Call the Peridigm proximity search routine ----

  selector = stk::mesh::Selector( metaData->universal_part() ) & ( stk::mesh::Selector( metaData->locally_owned_part() ) );
  std::vector<stk::mesh::Entity> nodes;
  stk::mesh::get_selected_entities(selector, bulkData->buckets(stk::topology::NODE_RANK), nodes);
  std::vector<double> proximitySearchCoords(3*nodes.size());
  std::vector<double> proximitySearchRadii(nodes.size());
  std::vector<int> globalIds(nodes.size());
  for(unsigned int iNode=0 ; iNode<nodes.size() ; ++iNode){

    double* coord = stk::mesh::field_data(*coordinatesField, nodes[iNode]);
    proximitySearchCoords[iNode*3]   = coord[0];
    proximitySearchCoords[iNode*3+1] = coord[1];
    proximitySearchCoords[iNode*3+2] = coord[2];

    bool isPeridynamicSphere(false);
    int numElementAttachedToNode = bulkData->num_elements(nodes[iNode]);
    if(numElementAttachedToNode == 1){
      const stk::mesh::Entity* elems = bulkData->begin_elements(nodes[iNode]);
      int numNodeInElement = bulkData->num_nodes(elems[0]);
      if(numNodeInElement == 1){
	isPeridynamicSphere = true;
      }
    }
    if(isPeridynamicSphere){
      proximitySearchRadii[iNode] = proximitySearchRadius;
    }
    else{
      proximitySearchRadii[iNode] = 0.0;
    }

    globalIds[iNode] = bulkData->identifier(nodes[iNode]) - 1;
  }

  const Teuchos::MpiComm<int>* mpiCommWrapper = dynamic_cast<const Teuchos::MpiComm<int>* >(teuchosComm.get());
  TEUCHOS_TEST_FOR_EXCEPT_MSG(mpiCommWrapper == 0, "\n\n**** Error in PeridigmManager::overlappingElementSearch(), failed to dynamically cast comm object to Teuchos::MpiComm<int>.\n");
  MPI_Comm mpiComm = static_cast<MPI_Comm>(*mpiCommWrapper->getRawMpiComm());
  TEUCHOS_TEST_FOR_EXCEPT_MSG(mpiComm == 0, "\n\n**** Error in PeridigmManager::overlappingElementSearch(), failed to dynamically cast comm object to MPI_Comm.\n");
  Epetra_MpiComm epetraComm(mpiComm);
  Epetra_BlockMap epetraOneDimensionalMap(-1,
					  static_cast<int>( nodes.size() ),
					  &globalIds[0],
					  1,
					  0,
					  epetraComm);
  Epetra_BlockMap epetraThreeDimensionalMap(-1,
					    static_cast<int>( nodes.size() ),
					    &globalIds[0],
					    3,
					    0,
					    epetraComm);

  // Input for proximity search routine
  Teuchos::RCP<Epetra_Vector> epetraProximitySearchCoords = Teuchos::rcp(new Epetra_Vector(epetraThreeDimensionalMap));
  Teuchos::RCP<Epetra_Vector> epetraProximitySearchRadii = Teuchos::rcp(new Epetra_Vector(epetraOneDimensionalMap));
  for(unsigned int iNode=0 ; iNode<nodes.size() ; ++iNode){
    (*epetraProximitySearchCoords)[3*iNode] = proximitySearchCoords[3*iNode];
    (*epetraProximitySearchCoords)[3*iNode+1] = proximitySearchCoords[3*iNode+1];
    (*epetraProximitySearchCoords)[3*iNode+2] = proximitySearchCoords[3*iNode+2];
    (*epetraProximitySearchRadii)[iNode] = proximitySearchRadii[iNode];
  }

  // Output from proximity search routine
  Teuchos::RCP<Epetra_BlockMap> overlapMap;
  int neighborListSize(0);
  int* neighborList(0);

  PeridigmNS::ProximitySearch::GlobalProximitySearch(epetraProximitySearchCoords,
						     epetraProximitySearchRadii,
						     overlapMap,
						     neighborListSize,
						     neighborList);

  // LOTS LEFT TO DO HERE...

  std::cout << "\n-- Overlapping Element Search --" << std::endl;
  std::cout << "  largest element dimension: " << largestElementDimension << std::endl;
  std::cout << "  proximity search radius: " << proximitySearchRadius << "\n" << std::endl;
}

void LCM::PeridigmManager::setCurrentTimeAndDisplacement(double time, const Teuchos::RCP<const Tpetra_Vector>& albanySolutionVector)
{
  if(hasPeridynamics){

    currentTime = time;
    timeStep = currentTime - previousTime;
    // Odd undefined things can happen if the time step is zero (e.g., if force is evaluated at time zero)
    // Hack around this situation.
    if(timeStep <= 0.0)
      timeStep = 1.0;
    peridigm->setTimeStep(timeStep);

    Epetra_Vector& peridigmReferencePositions = *(peridigm->getX());
    Epetra_Vector& peridigmCurrentPositions = *(peridigm->getY());
    Epetra_Vector& peridigmDisplacements = *(peridigm->getU());
    Epetra_Vector& peridigmVelocities = *(peridigm->getV());

    // Peridynamic elements (sphere elements)
    Teuchos::ArrayRCP<const ST> albanyCurrentDisplacements = albanySolutionVector->getData();
    const Teuchos::RCP<const Tpetra_Map> albanyMap = albanySolutionVector->getMap();
    Tpetra_Map::local_ordinal_type albanyLocalId;
    const Epetra_BlockMap& peridigmMap = peridigmCurrentPositions.Map();
    int peridigmLocalId, globalId;

    for(unsigned int i=0 ; i<sphereElementGlobalNodeIds.size() ; ++i){
      globalId = sphereElementGlobalNodeIds[i];
      peridigmLocalId = peridigmMap.LID(globalId);
      TEUCHOS_TEST_FOR_EXCEPT_MSG(peridigmLocalId == -1, "\n\n**** Error in PeridigmManager::setCurrentTimeAndDisplacement(), invalid Peridigm local id.\n\n");
      albanyLocalId = albanyMap->getLocalElement(3*globalId);
      TEUCHOS_TEST_FOR_EXCEPT_MSG(albanyLocalId == Teuchos::OrdinalTraits<LO>::invalid(), "\n\n**** Error in PeridigmManager::setCurrentTimeAndDisplacement(), invalid Albany local id.\n\n");
      peridigmDisplacements[3*peridigmLocalId]   = albanyCurrentDisplacements[albanyLocalId];
      peridigmDisplacements[3*peridigmLocalId+1] = albanyCurrentDisplacements[albanyLocalId+1];
      peridigmDisplacements[3*peridigmLocalId+2] = albanyCurrentDisplacements[albanyLocalId+2];
      peridigmCurrentPositions[3*peridigmLocalId]   = peridigmReferencePositions[3*peridigmLocalId] + peridigmDisplacements[3*peridigmLocalId];
      peridigmCurrentPositions[3*peridigmLocalId+1] = peridigmReferencePositions[3*peridigmLocalId+1] + peridigmDisplacements[3*peridigmLocalId+1];
      peridigmCurrentPositions[3*peridigmLocalId+2] = peridigmReferencePositions[3*peridigmLocalId+2] + peridigmDisplacements[3*peridigmLocalId+2];
      peridigmVelocities[3*peridigmLocalId]   = (peridigmCurrentPositions[3*peridigmLocalId]   - previousSolutionPositions[3*peridigmLocalId])/timeStep;
      peridigmVelocities[3*peridigmLocalId+1] = (peridigmCurrentPositions[3*peridigmLocalId+1] - previousSolutionPositions[3*peridigmLocalId+1])/timeStep;
      peridigmVelocities[3*peridigmLocalId+2] = (peridigmCurrentPositions[3*peridigmLocalId+2] - previousSolutionPositions[3*peridigmLocalId+2])/timeStep;
    }

    // Partial stress elements (solid elements with peridynamic material points at each integration point)

    for(std::vector<PartialStressElement>::iterator it=partialStressElements.begin() ; it!=partialStressElements.end() ; it++){

      // \todo This is brutal, need to store these data structures instead of re-creating them every time for every element.
      // Can probably store things by block and use worksets to compute things in one big call.

      // Create data structures for passing information to/from Intrepid.
      shards::CellTopology cellTopology(&it->cellTopologyData);
      Intrepid::DefaultCubatureFactory<RealType> cubFactory;
      Teuchos::RCP<Intrepid::Cubature<RealType> > cubature = cubFactory.create(cellTopology, cubatureDegree);
      const int numDim = cubature->getDimension();
      const int numQuadPoints = cubature->getNumPoints();
      const int numNodes = cellTopology.getNodeCount();
      const int numCells = 1;

      // Get the quadrature points and weights
      Intrepid::FieldContainer<RealType> quadratureRefPoints;
      Intrepid::FieldContainer<RealType> quadratureRefWeights;
      quadratureRefPoints.resize(numQuadPoints, numDim);
      quadratureRefWeights.resize(numQuadPoints);
      cubature->getCubature(quadratureRefPoints, quadratureRefWeights);

      typedef PHX::KokkosViewFactory<RealType, PHX::Device> ViewFactory;

      // Physical points, which are the physical (x, y, z) values of the quadrature points
      Teuchos::RCP< PHX::MDALayout<Cell, QuadPoint, Dim> > physPointsLayout = Teuchos::rcp(new PHX::MDALayout<Cell, QuadPoint, Dim>(numCells, numQuadPoints, numDim));
      PHX::MDField<RealType, Cell, QuadPoint, Dim> physPoints("Physical Points", physPointsLayout);
      physPoints.setFieldData(ViewFactory::buildView(physPoints.fieldTag()));

      // Reference points, which are the natural coordinates of the quadrature points
      Teuchos::RCP< PHX::MDALayout<Cell, QuadPoint, Dim> > refPointsLayout = Teuchos::rcp(new PHX::MDALayout<Cell, QuadPoint, Dim>(numCells, numQuadPoints, numDim));
      PHX::MDField<RealType, Cell, QuadPoint, Dim> refPoints("Reference Points", refPointsLayout);
      refPoints.setFieldData(ViewFactory::buildView(refPoints.fieldTag()));

      // Cell workset, which is the set of nodes for the given element
      Teuchos::RCP< PHX::MDALayout<Cell, Node, Dim> > cellWorksetLayout = Teuchos::rcp(new PHX::MDALayout<Cell, Node, Dim>(numCells, numNodes, numDim));
      PHX::MDField<RealType, Cell, Node, Dim> cellWorkset("Cell Workset", cellWorksetLayout);
      cellWorkset.setFieldData(ViewFactory::buildView(cellWorkset.fieldTag()));

      // Copy the reference points from the Intrepid::FieldContainer to a PHX::MDField
      for(int qp=0 ; qp<numQuadPoints ; ++qp){
	for(int dof=0 ; dof<3 ; ++dof){
	  refPoints(0, qp, dof) = quadratureRefPoints(qp, dof);
	}
      }

      int numNodesInElement = bulkData->num_nodes(it->albanyElement);
      const stk::mesh::Entity* node = bulkData->begin_nodes(it->albanyElement);
      Teuchos::ArrayRCP<const ST> albanyCurrentDisplacement = albanySolutionVector->getData();

      for(int i=0 ; i<numNodesInElement ; i++){
	int globalAlbanyNodeId = bulkData->identifier(node[i]) - 1;
	int albanyCurrentDisplacementIndex = albanyMap->getLocalElement(3*globalAlbanyNodeId);
	cellWorkset(0, i, 0) = it->albanyNodeInitialPositions[3*i]   + albanyCurrentDisplacement[albanyCurrentDisplacementIndex];
	cellWorkset(0, i, 1) = it->albanyNodeInitialPositions[3*i+1] + albanyCurrentDisplacement[albanyCurrentDisplacementIndex + 1];
	cellWorkset(0, i, 2) = it->albanyNodeInitialPositions[3*i+2] + albanyCurrentDisplacement[albanyCurrentDisplacementIndex + 2];
      }

      // Determine the global (x,y,z) coordinates of the quadrature points
      Intrepid::CellTools<RealType>::mapToPhysicalFrame(physPoints, refPoints, cellWorkset, cellTopology);

      for(unsigned int i=0 ; i<it->peridigmGlobalIds.size() ; ++i){
	peridigmLocalId = peridigmGlobalIdToPeridigmLocalId[it->peridigmGlobalIds[i]];
	TEUCHOS_TEST_FOR_EXCEPT_MSG(peridigmLocalId == -1, "\n\n**** Error in PeridigmManager::setCurrentTimeAndDisplacement(), invalid Peridigm local id.\n\n");
	peridigmCurrentPositions[3*peridigmLocalId]   = physPoints(0, i, 0);
	peridigmCurrentPositions[3*peridigmLocalId+1] = physPoints(0, i, 1);
	peridigmCurrentPositions[3*peridigmLocalId+2] = physPoints(0, i, 2);
	peridigmDisplacements[3*peridigmLocalId]   = peridigmCurrentPositions[3*peridigmLocalId]   - peridigmReferencePositions[3*peridigmLocalId];
	peridigmDisplacements[3*peridigmLocalId+1] = peridigmCurrentPositions[3*peridigmLocalId+1] - peridigmReferencePositions[3*peridigmLocalId+1];
	peridigmDisplacements[3*peridigmLocalId+2] = peridigmCurrentPositions[3*peridigmLocalId+2] - peridigmReferencePositions[3*peridigmLocalId+2];
	peridigmVelocities[3*peridigmLocalId]   = (peridigmCurrentPositions[3*peridigmLocalId]   - previousSolutionPositions[3*peridigmLocalId])/timeStep;
	peridigmVelocities[3*peridigmLocalId+1] = (peridigmCurrentPositions[3*peridigmLocalId+1] - previousSolutionPositions[3*peridigmLocalId+1])/timeStep;
	peridigmVelocities[3*peridigmLocalId+2] = (peridigmCurrentPositions[3*peridigmLocalId+2] - previousSolutionPositions[3*peridigmLocalId+2])/timeStep;
      }
    }
  }
}

void LCM::PeridigmManager::updateState()
{
  if(hasPeridynamics){
    previousTime = currentTime;
    const Teuchos::RCP<const Epetra_Vector> peridigmY = peridigm->getY();
    for(unsigned int i=0 ; i<previousSolutionPositions.size() ; ++i)
      previousSolutionPositions[i] = (*peridigmY)[i];
    peridigm->updateState();
  }
}

void LCM::PeridigmManager::writePeridigmSubModel(RealType currentTime)
{
  if(hasPeridynamics)
    peridigm->writePeridigmSubModel(currentTime);
}

void LCM::PeridigmManager::evaluateInternalForce()
{
  if(hasPeridynamics)
    peridigm->computeInternalForce();
}

double LCM::PeridigmManager::getForce(int globalAlbanyNodeId, int dof)
{
  double force(0.0);

  if(hasPeridynamics){
    Epetra_Vector& peridigmForce = *(peridigm->getForce());
    int peridigmLocalId = peridigmForce.Map().LID(globalAlbanyNodeId);
    TEUCHOS_TEST_FOR_EXCEPT_MSG(peridigmLocalId == -1, "\n\n**** Error in PeridigmManager::getForce(), invalid global id.\n\n");
    force = peridigmForce[3*peridigmLocalId + dof];
  }
  return force;
}

void LCM::PeridigmManager::getPartialStress(std::string blockName, int worksetIndex, int worksetLocalElementId, std::vector< std::vector<RealType> >& partialStressValues)
{
  if(hasPeridynamics){

    int globalElementId = worksetLocalIdToGlobalId[worksetIndex][worksetLocalElementId];
    std::vector<int>& peridigmGlobalIds = albanyPartialStressElementGlobalIdToPeridigmGlobalIds[globalElementId];
    Teuchos::RCP<const Epetra_Vector> data = peridigm->getBlockData(blockName, "Partial_Stress");
    Teuchos::RCP<const Epetra_Vector> displacement = peridigm->getBlockData(blockName, "Displacement");
    for(unsigned int i=0 ; i<peridigmGlobalIds.size() ; ++i){
      int peridigmLocalId = data->Map().LID(peridigmGlobalIds[i]);
      TEUCHOS_TEST_FOR_EXCEPT_MSG(peridigmLocalId == -1, "\n\n**** Error in PeridigmManager::getPartialStress(), invalid global id.\n");
      for(int j=0 ; j<9 ; ++j){
	partialStressValues[i][j] = (*data)[9*peridigmLocalId + j];
      }
    }
  }
}

Teuchos::RCP<const Epetra_Vector> LCM::PeridigmManager::getBlockData(std::string blockName, std::string fieldName)
{

  Teuchos::RCP<const Epetra_Vector> data;

  if(hasPeridynamics)
    data = peridigm->getBlockData(blockName, fieldName);

  return data;
}

void LCM::PeridigmManager::setOutputFields(const Teuchos::ParameterList& params)
{
  for(Teuchos::ParameterList::ConstIterator it = params.begin(); it != params.end(); ++it) {
    std::string name = it->first;

    // Hard-code everything for the initial implementation
    // It would be best to just use the PeridigmNS::FieldManager to figure out if
    // a variable is a global, nodal, or element variable and if it is scalar or vector
    // But there is an order-of-operations issue; the PeridigmNS::FieldManager has not
    // been instantiated yet, so at this point it is empty

    OutputField field;
    field.albanyName = "Peridigm_" + name;
    field.peridigmName = name;

    if(name == "Dilatation" || name == "Weighted_Volume" || name == "Radius" || name == "Number_Of_Neighbors" || name == "Horizon" || name == "Volume"){
      field.relation = "element";
      field.initType = "scalar";
      field.length = 1;
    }
    else if(name == "Model_Coordinates" || name == "Coordinates" || name == "Displacement" || name == "Velocity" || name == "Force"){
      field.relation = "node";
      field.initType = "scalar";
      field.length = 3;
    }
    else if(name == "Deformation_Gradient" || name == "Unrotated_Rate_Of_Deformation" || name == "Cauchy_Stress" || name == "Partial_Stress"){
      field.relation = "element";
      field.initType = "scalar";
      field.length = 9;
    }
    else{
      TEUCHOS_TEST_FOR_EXCEPT_MSG(true, "\n\n**** Error in PeridigmManager::setOutputVariableList(), unknown variable.  All variables must be hard-coded in PeridigmManager.cpp (sad but true).\n\n");
    }

    if( std::find(outputFields.begin(), outputFields.end(), field) == outputFields.end() )
      outputFields.push_back(field);
  }
}

std::vector<LCM::PeridigmManager::OutputField> LCM::PeridigmManager::getOutputFields()
{
  return outputFields;
}
