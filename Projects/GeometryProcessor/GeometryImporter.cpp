//////////////////////////////////////////////////////////////////////////
/// Authors: Dane Curbow
/// Copyright 2016, DigiPen Institute of Technology
//////////////////////////////////////////////////////////////////////////
#include "Precompiled.hpp"

namespace Zero
{

GeometryImporter::GeometryImporter(String inputFile, String outputPath, String metaFile)
  : mScene(nullptr),
    mInputFile(inputFile),
    mOutputPath(outputPath),
    mMetaFile(metaFile),
    mBaseMeshName(FilePath::GetFileNameWithoutExtension(inputFile)),
    mUniquifyingIndex(0)
{
}

GeometryImporter::~GeometryImporter()
{
  delete mGeometryContent;
}

uint GeometryImporter::SetupAssimpPostProcess()
{
  MeshBuilder* meshBuilder = mGeometryContent->has(MeshBuilder);
  if (meshBuilder == nullptr)
    return 0;

  uint flags = 0;
  flags |= aiProcess_JoinIdenticalVertices;
  // always turn polygons into triangle
  flags |= aiProcess_Triangulate;
  // always generate normals if none are present
  flags |= aiProcess_GenSmoothNormals;

  int removeFlags = 0;

  if (meshBuilder->mInvertUvYAxis)
  {
    flags |= aiProcess_FlipUVs;
  }

  if (meshBuilder->mGenerateSmoothNormals)
  {
    // if generate normals is checked we want to remove any existing normals and generate our own
    flags |= aiProcess_RemoveComponent;
    removeFlags |= aiComponent_NORMALS;
    mAssetImporter.SetPropertyFloat(AI_CONFIG_PP_GSN_MAX_SMOOTHING_ANGLE, meshBuilder->mSmoothingAngleDegreesThreshold);
  }

  if (meshBuilder->mGenerateTangentSpace)
  {
    flags |= aiProcess_CalcTangentSpace;
    flags |= aiProcess_RemoveComponent;
    removeFlags |= aiComponent_TANGENTS_AND_BITANGENTS;
  }

  if (meshBuilder->mFlipWindingOrder)
  {
    flags |= aiProcess_FlipWindingOrder;
  }

  mAssetImporter.SetPropertyInteger(AI_CONFIG_PP_RVC_FLAGS, removeFlags);

  return flags;
}

int GeometryImporter::ProcessModelFiles()
{
  // Load the all the import options
  if (FileExists(mMetaFile.c_str()))
  {
    // Load from meta file
    mGeometryContent = new GeometryContent(mInputFile);
    LoadFromDataFile(*mGeometryContent, mMetaFile);
  }
  else
    return GeometryProcessorCodes::Failed;

  // set the flags for post process we want to run
  uint flags = SetupAssimpPostProcess();

  // load in the file
  mScene = mAssetImporter.ReadFile(mInputFile.c_str(), flags);
  ZPrint("Processing model: %s\n", FilePath::GetFileNameWithoutExtension(mInputFile).Data());

  // An error has occurred, no scene imported
  if (!mScene)
  {
    String error(mAssetImporter.GetErrorString());
    ZPrint("Geometry Processor Error: %s\n", error.c_str());
    return Zero::GeometryProcessorCodes::Failed;
  }

  if (SceneEmpty())
    return GeometryProcessorCodes::NoContent;

  CollectNodeData();
  bool metaUpdated = UpdateBuilderMetaData();

  mGeometryContent->has(GeometryImport)->ComputeTransforms();

  // Assimp stores all bone data on the meshes so if there are no meshes, there are no skeletons
  if (mScene->HasMeshes())
  {
    // We need the hierarchy data to travel through it marking what nodes comprise the skeleton
    SkeletonProcessor skeletonProcess(mHierarchyDataMap, mMeshDataMap, mRootNodeName);
    skeletonProcess.ProcessSkeletonHierarchy(mScene);
  }

  // process the data into our format and export the files
  MeshBuilder* meshBuilder = mGeometryContent->has(MeshBuilder);
  if (meshBuilder && mScene->HasMeshes())
  {
    MeshProcessor meshProcessor(meshBuilder, mMeshDataMap);
    meshProcessor.ExtractAndProcessMeshData(mScene);
    meshProcessor.ExportMeshData(mOutputPath);
  }
  
  PhysicsMeshBuilder* physicsMeshBuilder = mGeometryContent->has(PhysicsMeshBuilder);
  if(physicsMeshBuilder)
  {
    PhysicsMeshProcessor physicsMeshProcessor(physicsMeshBuilder, mMeshDataMap);
    physicsMeshProcessor.BuildPhysicsMesh(mOutputPath);
  }
  
  TextureContent* textureContent = mGeometryContent->has(TextureContent);
  if (mScene->HasTextures() && textureContent)
  {
    TextureProcessor textureProcessor(textureContent, mOutputPath, mInputFile);
    textureProcessor.ExtractAndImportTextures(mScene);
  }

  AnimationBuilder* animationBuilder = mGeometryContent->has(AnimationBuilder);
  if (animationBuilder && mScene->HasAnimations())
  {
    animationBuilder->Name = mBaseMeshName;
    AnimationProcessor animationProcessor(animationBuilder, mHierarchyDataMap);
    animationProcessor.ExtractAndProcessAnimationData(mScene);
    animationProcessor.ExportAnimationData(mOutputPath);
  }

  // we need to build a scene graph such that on import the engine can create an archetype for
  // this mesh file and any animations attached to it if it has any
  GeneratedArchetype* generatedArchetype = mGeometryContent->has(GeneratedArchetype);
  if (generatedArchetype)
  {
    ArchetypeProcessor archetypeProcessor(generatedArchetype, mHierarchyDataMap);
    archetypeProcessor.BuildSceneGraph(mRootNodeName);
    archetypeProcessor.ExportSceneGraph(FilePath::GetFileName(mInputFile), mOutputPath);
  }

  if ((metaUpdated || animationBuilder || generatedArchetype) && !textureContent)
    return GeometryProcessorCodes::LoadGraph;

  if (!(metaUpdated || animationBuilder) && textureContent)
    return GeometryProcessorCodes::LoadTextures;

  if ((metaUpdated || animationBuilder || generatedArchetype) && textureContent)
    return GeometryProcessorCodes::LoadGraphAndTextures;

  return GeometryProcessorCodes::Success;
}

bool GeometryImporter::SceneEmpty()
{
  // nothing we import is present, the scene is empty
  if (!mScene->HasAnimations() && !mScene->HasMeshes() && !mScene->HasTextures())
    return true;

  return false;
}

void GeometryImporter::CollectNodeData()
{
  // loop through the scenes nodes children and find the ones with a mesh attached and
  // store them along with their name for process and export
  aiNode* rootNode = mScene->mRootNode;
  mRootNodeName = CleanAssetName(rootNode->mName.C_Str());
  ExtractDataFromNodesRescursive(rootNode, "");

  ComputeMeshTransforms();
}

String GeometryImporter::ExtractDataFromNodesRescursive(aiNode* node, String parentName)
{
  uint numChildren = node->mNumChildren;

  // collect the hierarchy data, used for animations if the scene has any
  String nodeName = CleanAssetName(node->mName.C_Str());
  // check to see if this node name is a unique entry, if no append an number to the end to make it unique
  if (mHierarchyDataMap.ContainsKey(nodeName))
    nodeName = BuildString(nodeName, ToString(mUniquifyingIndex++));

  String nodePath = nodeName;
  if (node->mParent)
    nodePath = BuildString(mHierarchyDataMap[parentName].mNodePath, cAnimationPathDelimiterStr, nodeName);

  HierarchyData hierarchyData;
  hierarchyData.mParentNodeName = parentName;
  hierarchyData.mNodeName = nodeName;
  hierarchyData.mNodePath = nodePath;
  hierarchyData.mLocalTransform = AiMat4ToZeroMat4(node->mTransformation);
  
  // if the node has a mesh store the mesh keyed to its name
  if (node->mNumMeshes == 1)
  {
    // store one mesh entry on this node
    SingleMeshHierarchyEntry(hierarchyData, node->mMeshes[0]);
  }
  else if (node->mNumMeshes > 1)
  {
    // create a child node for each mesh
    MultipleMeshsHierarchicalEntry(hierarchyData, node);
  }

  mHierarchyDataMap.Insert(nodeName, hierarchyData);
  
  // walk down all the children
  for (uint i = 0; i < numChildren; ++i)
  {
    String childsName = ExtractDataFromNodesRescursive(node->mChildren[i], nodeName);
    mHierarchyDataMap[nodeName].mChildren.PushBack(childsName);
  }

  return nodeName;
}

void GeometryImporter::SingleMeshHierarchyEntry(HierarchyData& hierarchyData, uint meshIndex)
{
  hierarchyData.mHasMesh = true;

  if (mMeshDataMap.ContainsKey(meshIndex))
  {
    hierarchyData.mMeshName = mMeshDataMap[meshIndex].mMeshName;
  }
  else
  {
    MeshData data;
    data.mMeshTransform = hierarchyData.mLocalTransform;
    hierarchyData.mMeshName = data.mMeshName = hierarchyData.mNodeName;
    mMeshDataMap.Insert(meshIndex, data);
  }
}

void GeometryImporter::MultipleMeshsHierarchicalEntry(HierarchyData& hierarchyData, aiNode* node)
{
  String parentNodeName = hierarchyData.mNodeName;

  for (size_t i = 0; i < node->mNumMeshes; ++i)
  {
    HierarchyData childHierarchyData;
    childHierarchyData.mParentNodeName = parentNodeName;
    childHierarchyData.mNodeName = BuildString(parentNodeName, "GeneratedMeshNode", ToString(mUniquifyingIndex++));
    childHierarchyData.mNodePath = BuildString(hierarchyData.mNodePath, cAnimationPathDelimiterStr, childHierarchyData.mNodeName);
    childHierarchyData.mLocalTransform = Mat4::cIdentity;

    SingleMeshHierarchyEntry(childHierarchyData, node->mMeshes[i]);

    hierarchyData.mChildren.PushBack(childHierarchyData.mNodeName);
    mHierarchyDataMap.Insert(childHierarchyData.mNodeName, childHierarchyData);
  }
}

void GeometryImporter::ComputeMeshTransforms()
{
  forRange (HierarchyData& node, mHierarchyDataMap.Values())
  {
    if (node.mHasMesh == false)
      continue;

    Mat4 meshTransform = node.mLocalTransform;
    String parentName = node.mParentNodeName;
    
    HierarchyData parentNode = mHierarchyDataMap[parentName];
    while (!parentNode.mParentNodeName.Empty())
    {
      parentNode = mHierarchyDataMap[parentName];
      meshTransform = parentNode.mLocalTransform * meshTransform;
      parentName = parentNode.mParentNodeName;
    }

    forRange (MeshData& meshData, mMeshDataMap.Values())
    {
      if (meshData.mMeshName == node.mMeshName)
      {
        meshData.mMeshTransform = meshTransform;
        break;
      }
    }
  }
}

bool GeometryImporter::UpdateBuilderMetaData()
{
  bool metaChanges = false;

  MeshBuilder* meshBuilder = mGeometryContent->has(MeshBuilder);
  if (mScene->HasMeshes() && meshBuilder != nullptr)
  {
    Array<GeometryResourceEntry> meshEntries;

    uint meshCount = mScene->mNumMeshes;
    for (uint i = 0; i < meshCount; ++i)
    {
      if (!mMeshDataMap.ContainsKey(i))
        continue;

      String name = mBaseMeshName;
      if (meshCount > 1)
        name = BuildString(name, "_", mMeshDataMap[i].mMeshName);

      GeometryResourceEntry entry;
      entry.mName = name;

      if (GeometryResourceEntry* previousEntry = meshBuilder->Meshes.FindPointer(entry))
        entry.mResourceId = previousEntry->mResourceId;
      else
        entry.mResourceId = GenerateUniqueId64();

      meshEntries.PushBack(entry);
    }

    if (meshBuilder->Meshes != meshEntries)
    {
      meshBuilder->Meshes = meshEntries;
      metaChanges = true;
    }
  }

  PhysicsMeshBuilder* physicsMeshBuilder = mGeometryContent->has(PhysicsMeshBuilder);
  if (mScene->HasMeshes() && physicsMeshBuilder != nullptr)
  {
    Array<GeometryResourceEntry> physicsMeshEntries;

    uint meshCount = mScene->mNumMeshes;
    for (uint i = 0; i < meshCount; ++i)
    {
      if (!mMeshDataMap.ContainsKey(i))
        continue;

      String name = mBaseMeshName;
      if (meshCount > 1)
        name = BuildString(name, "_", mMeshDataMap[i].mMeshName);

      GeometryResourceEntry entry;
      entry.mName = name;

      if (GeometryResourceEntry* previousEntry = physicsMeshBuilder->Meshes.FindPointer(entry))
        entry.mResourceId = previousEntry->mResourceId;
      else
        entry.mResourceId = GenerateUniqueId64();

      physicsMeshEntries.PushBack(entry);
    }

    if (physicsMeshBuilder->Meshes != physicsMeshEntries)
    {
      physicsMeshBuilder->Meshes = physicsMeshEntries;
      metaChanges = true;
    }
  }

  AnimationBuilder* animBuilder = mGeometryContent->has(AnimationBuilder);
  if (mScene->HasAnimations() && animBuilder != nullptr)
  {
    aiAnimation** animations = mScene->mAnimations;
    uint animationCount = mScene->mNumAnimations;

    Array<GeometryResourceEntry> animEntries;

    if (animBuilder->mClips.Size() != 0)
    {
      forRange (AnimationClip& clip, animBuilder->mClips.All())
      {
        if ((size_t)clip.mAnimationIndex >= animationCount)
          continue;

        String name = BuildString(mBaseMeshName, "_", CleanAssetName(clip.mName));

        GeometryResourceEntry entry;
        entry.mName = name;

        // Ignore attempts to make duplicate names.
        if (animEntries.Contains(entry))
          continue;

        // Get resource id if this name already had one, otherwise make a new one.
        if (GeometryResourceEntry* previousEntry = animBuilder->mAnimations.FindPointer(entry))
          entry.mResourceId = previousEntry->mResourceId;
        else
          entry.mResourceId = GenerateUniqueId64();

        animEntries.PushBack(entry);
      }
    }
    else
    {
      for (uint i = 0; i < animationCount; ++i)
      {
        aiAnimation* animation = animations[i];

        String name = mBaseMeshName;
        if (animationCount > 1)
          name = BuildString(name, "_", CleanAssetName(animation->mName.C_Str()));

        GeometryResourceEntry entry;
        entry.mName = name;

        // Get resource id if this name already had one, otherwise make a new one.
        if (GeometryResourceEntry* previousEntry = animBuilder->mAnimations.FindPointer(entry))
          entry.mResourceId = previousEntry->mResourceId;
        else
          entry.mResourceId = GenerateUniqueId64();

        animEntries.PushBack(entry);
      }
    }

    if (animBuilder->mAnimations != animEntries)
    {
      animBuilder->mAnimations = animEntries;
      metaChanges = true;
    }
  }

  TextureContent* textureContent = mGeometryContent->has(TextureContent);
  if (mScene->HasTextures() && textureContent != nullptr)
  {
    aiTexture** textures = mScene->mTextures;
    uint textureCount = mScene->mNumTextures;

    Array<GeometryResourceEntry> textureEntries;

    for (uint i = 0; i < textureCount; ++i)
    {
      aiTexture* texture = textures[i];

      if (!(texture->mHeight == 0 && texture->CheckFormat("png")))
        continue;

      GeometryResourceEntry entry;
      entry.mName = BuildString(FilePath::GetFileNameWithoutExtension(mInputFile), ToString(i), ".png");

      // Get resource id if this name already had one, otherwise make a new one.
      if (GeometryResourceEntry* previousEntry = textureContent->mTextures.FindPointer(entry))
        entry.mResourceId = previousEntry->mResourceId;
      else
        entry.mResourceId = GenerateUniqueId64();

      textureEntries.PushBack(entry);
    }

    if (textureContent->mTextures != textureEntries)
    {
      textureContent->mTextures = textureEntries;
      metaChanges = true;
    }
  }

  if (metaChanges)
    SaveToDataFile(*mGeometryContent, mMetaFile);
  return metaChanges;
}

//   void GeometryImporter::GenerateNormals(MeshData& meshData)
//   {
//     // zero out all the existing normals or garbage data
//     size_t vertexBufferSize = meshData.mVertexBuffer.Size();
//     for (size_t i = 0; i < vertexBufferSize; ++i)
//       meshData.mVertexBuffer[i].mNormal = Vec3(0, 0, 0);
// 
//     Array<float> vertexInfluence;
//     vertexInfluence.Resize(vertexBufferSize, 0.0f);
//     // loop through and generate all the face normals
//     size_t indexBufferSize = meshData.mIndexBuffer.Size();
//     for (size_t i = 0; i < indexBufferSize; i += 3)
//     {
//       uint indexA = meshData.mIndexBuffer[i];
//       uint indexB = meshData.mIndexBuffer[i + 1];
//       uint indexC = meshData.mIndexBuffer[i + 2];
//       VertexData& vertexA = meshData.mVertexBuffer[indexA];
//       VertexData& vertexB = meshData.mVertexBuffer[indexB];
//       VertexData& vertexC = meshData.mVertexBuffer[indexC];
// 
//       Vec3 faceNormal = Geometry::GenerateNormal(vertexA.mPosition, vertexB.mPosition, vertexC.mPosition);
//       vertexA.mNormal += faceNormal;
//       vertexB.mNormal += faceNormal;
//       vertexC.mNormal += faceNormal;
//       vertexInfluence[indexA] += 1.f;
//       vertexInfluence[indexB] += 1.f;
//       vertexInfluence[indexC] += 1.f;
//     }
// 
//     for (size_t i = 0; i < vertexBufferSize; ++i)
//       meshData.mVertexBuffer[i].mNormal /= vertexInfluence[i];
// 
//   }

//   void GeometryImporter::SmoothNormals(MeshData& meshData)
//   {
//     MeshBuilder* meshBuilder = mGeometryContent->has(MeshBuilder);
//     float smoothingThresholdRadians = Math::DegToRad(meshBuilder->mSmoothingAngleDegreesThreshold);
// 
//     typedef  HashMap<Vec3, Array<uint>> VertexMultiMap;
//     VertexMultiMap vertexDataMultiMap;
// 
//     // collect all vertices at the same position into arrays
//     size_t vertexBufferSize = meshData.mVertexBuffer.Size();
//     for (size_t i = 0; i < vertexBufferSize; ++i)
//       vertexDataMultiMap[meshData.mVertexBuffer[i].mPosition].PushBack(i);
// 
//     VertexMultiMap::valuerange range = vertexDataMultiMap.Values();
//     // go over all the arrays of collected alike vertices
//     while (!range.Empty())
//     {
//       Array<uint>& alikeVertices = range.Front();
//       range.PopFront();
// 
//       while (alikeVertices.Size() > 1)
//       {
//         size_t indexA = alikeVertices.Back();
//         alikeVertices.PopBack();
//         size_t indexB = alikeVertices.Back();
//         alikeVertices.PopBack();
//         VertexData& vertexA = meshData.mVertexBuffer[indexA];
//         VertexData& vertexB = meshData.mVertexBuffer[indexB];
//         ErrorIf(vertexA.mPosition != vertexB.mPosition, "Two vertices considered the same are different");
//         if (acos(vertexA.mNormal.Dot(vertexB.mNormal)) <= smoothingThresholdRadians)
//         {
//           Vec3 smoothedNormal = vertexA.mNormal + vertexB.mNormal;
//           smoothedNormal /= 2.f;
//           smoothedNormal.Normalize();
//           vertexA.mNormal = vertexB.mNormal = smoothedNormal;
//         }
//         alikeVertices.PushBack(indexB);
//       }
//     }
//   }

}// namespace Zero