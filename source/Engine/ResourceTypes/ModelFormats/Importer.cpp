#if INTERFACE
#include <Engine/IO/Stream.h>
#include <Engine/Rendering/Material.h>
#include <Engine/ResourceTypes/IModel.h>
class ModelImporter {
public:
    static vector<int> MeshIDs;
    static char*       ParentDirectory;
};
#endif

#include <Engine/ResourceTypes/ModelFormats/Importer.h>
#include <Engine/Includes/Standard.h>
#include <Engine/Rendering/3D.h>
#include <Engine/Math/Matrix4x4.h>
#include <Engine/Utilities/StringUtils.h>

vector<int> ModelImporter::MeshIDs;
char*       ModelImporter::ParentDirectory;

#ifdef USING_ASSIMP
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#define LOG_FMT(s) \
    char s[1024]; \
    va_list args; \
    va_start(args, format); \
    vsnprintf(s, sizeof s, format, args); \
    va_end(args); \
    s[sizeof(s) - 1] = 0

static void LogWarn(const char* format, ...) {
    LOG_FMT(string);
    Log::Print(Log::LOG_WARN, "Model importer: %s", string);
}

static void LogError(const char* format, ...) {
    LOG_FMT(string);
    Log::Print(Log::LOG_ERROR, "Model importer: %s", string);
}

static void LogVerbose(const char* format, ...) {
    LOG_FMT(string);
    Log::Print(Log::LOG_VERBOSE, "Model importer: %s", string);
}

static void CopyColors(float dest[4], aiColor4D& src) {
    dest[0] = src.r;
    dest[1] = src.g;
    dest[2] = src.b;
    dest[3] = src.a;
}

static char* GetString(aiString src) {
    return StringUtils::Duplicate(src.C_Str());
}

static Matrix4x4* CopyMatrix(aiMatrix4x4 mat) {
    Matrix4x4* out = Matrix4x4::Create();

    for (size_t i = 0; i < 16; i++)
        out->Values[i] = mat[i >> 2][i & 3];

    return out;
}

static AnimBehavior ConvertPrePostState(aiAnimBehaviour state) {
    switch (state) {
        case aiAnimBehaviour_CONSTANT:
            return AnimBehavior_CONSTANT;
        case aiAnimBehaviour_LINEAR:
            return AnimBehavior_LINEAR;
        case aiAnimBehaviour_REPEAT:
            return AnimBehavior_REPEAT;
        default:
            return AnimBehavior_DEFAULT;
    }
}

static AnimVectorKey GetVectorKey(struct aiVectorKey* vecKey) {
    AnimVectorKey key;

    key.Time = vecKey->mTime;
    key.Value.X = (int)(vecKey->mValue.x * 0x10000);
    key.Value.Y = (int)(vecKey->mValue.y * 0x10000);
    key.Value.Z = (int)(vecKey->mValue.z * 0x10000);

    return key;
}

static AnimQuaternionKey GetQuatKey(struct aiQuatKey* quatKey) {
    AnimQuaternionKey key;

    key.Time = quatKey->mTime;
    key.Value.X = (int)(quatKey->mValue.x * 0x10000);
    key.Value.Y = (int)(quatKey->mValue.y * 0x10000);
    key.Value.Z = (int)(quatKey->mValue.z * 0x10000);
    key.Value.W = (int)(quatKey->mValue.w * 0x10000);

    return key;
}

PRIVATE STATIC Mesh* ModelImporter::LoadMesh(IModel* imodel, struct aiMesh* amesh) {
    size_t numFaces = amesh->mNumFaces;
    size_t numVertices = amesh->mNumVertices;

    Mesh* mesh = new Mesh;
    mesh->Name = GetString(amesh->mName);
    mesh->VertexCount = numVertices;

    mesh->VertexIndexCount = numFaces * 3;
    mesh->VertexIndexBuffer = (Sint32*)Memory::Malloc((mesh->VertexIndexCount + 1) * sizeof(Sint32));

    mesh->MaterialIndex = (int)amesh->mMaterialIndex;

    Vector3* vert;
    Vector3* norm = nullptr;
    Vector2* uv = nullptr;
    Uint32* color = nullptr;

    mesh->VertexFlag = VertexType_Position;
    mesh->PositionBuffer = vert = (Vector3*)Memory::Malloc(numVertices * sizeof(Vector3));

    if (amesh->HasNormals()) {
        mesh->VertexFlag |= VertexType_Normal;
        mesh->NormalBuffer = norm = (Vector3*)Memory::Malloc(numVertices * sizeof(Vector3));
    }

    if (amesh->HasTextureCoords(0)) {
        mesh->VertexFlag |= VertexType_UV;
        mesh->UVBuffer = uv = (Vector2*)Memory::Malloc(numVertices * sizeof(Vector2));
    }

    if (amesh->HasVertexColors(0)) {
        mesh->VertexFlag |= VertexType_Color;
        mesh->ColorBuffer = color = (Uint32*)Memory::Malloc(numVertices * sizeof(Uint32));
        for (int i = 0; i < numVertices; i++)
            mesh->ColorBuffer[i] = 0xFFFFFFFF;
    }

    for (size_t v = 0; v < numVertices; v++) {
        vert->X = (int)(amesh->mVertices[v].x * 0x10000);
        vert->Y = (int)(amesh->mVertices[v].y * 0x10000);
        vert->Z = (int)(amesh->mVertices[v].z * 0x10000);

        vert++;

        if (mesh->VertexFlag & VertexType_Normal) {
            norm->X = (int)(amesh->mNormals[v].x * 0x10000);
            norm->Y = (int)(amesh->mNormals[v].y * 0x10000);
            norm->Z = (int)(amesh->mNormals[v].z * 0x10000);

            norm++;
        }

        if (mesh->VertexFlag & VertexType_UV) {
            uv->X = (int)(amesh->mTextureCoords[0][v].x * 0x10000);
            uv->Y = (int)(amesh->mTextureCoords[0][v].y * 0x10000);
            uv++;
        }

        if (mesh->VertexFlag & VertexType_Color) {
            int r = (int)(amesh->mColors[0][v].r * 0xFF);
            int g = (int)(amesh->mColors[0][v].g * 0xFF);
            int b = (int)(amesh->mColors[0][v].b * 0xFF);
            int a = (int)(amesh->mColors[0][v].a * 0xFF);
            *color = a << 24 | r << 16 | g << 8 | b;
            color++;
        }
    }

    for (size_t f = 0; f < numFaces; f++) {
        struct aiFace* face = &amesh->mFaces[f];
        mesh->VertexIndexBuffer[f * 3]     = face->mIndices[0];
        mesh->VertexIndexBuffer[f * 3 + 1] = face->mIndices[1];
        mesh->VertexIndexBuffer[f * 3 + 2] = face->mIndices[2];
    }

    mesh->VertexIndexBuffer[mesh->VertexIndexCount] = -1;
    imodel->VertexIndexCount += mesh->VertexIndexCount;

    return mesh;
}

PRIVATE STATIC Material* ModelImporter::LoadMaterial(IModel* imodel, struct aiMaterial* mat) {
    Material* material = new Material();

    aiString texDiffuse;
    aiColor4D colorDiffuse;
    aiColor4D colorSpecular;
    aiColor4D colorAmbient;
    aiColor4D colorEmissive;
    ai_real shininess, shininessStrength, opacity;
    unsigned int n = 1;

    if (mat->GetTexture(aiTextureType_DIFFUSE, 0, &texDiffuse) == AI_SUCCESS)
        material->ImagePtr = IModel::LoadMaterialImage(texDiffuse.data, ModelImporter::ParentDirectory);

    if (aiGetMaterialColor(mat, AI_MATKEY_COLOR_DIFFUSE, &colorDiffuse) == AI_SUCCESS)
        CopyColors(material->Diffuse, colorDiffuse);
    if (aiGetMaterialColor(mat, AI_MATKEY_COLOR_SPECULAR, &colorSpecular) == AI_SUCCESS)
        CopyColors(material->Specular, colorSpecular);
    if (aiGetMaterialColor(mat, AI_MATKEY_COLOR_AMBIENT, &colorAmbient) == AI_SUCCESS)
        CopyColors(material->Ambient, colorAmbient);
    if (aiGetMaterialColor(mat, AI_MATKEY_COLOR_EMISSIVE, &colorEmissive) == AI_SUCCESS)
        CopyColors(material->Emissive, colorEmissive);

    if (aiGetMaterialFloatArray(mat, AI_MATKEY_SHININESS, &shininess, &n) == AI_SUCCESS)
        material->Shininess = shininess;
    if (aiGetMaterialFloatArray(mat, AI_MATKEY_SHININESS_STRENGTH, &shininessStrength, &n) == AI_SUCCESS)
        material->ShininessStrength = shininessStrength;
    if (aiGetMaterialFloatArray(mat, AI_MATKEY_OPACITY, &opacity, &n) == AI_SUCCESS)
        material->Opacity = opacity;

    return material;
}

PRIVATE STATIC ModelNode* ModelImporter::LoadNode(IModel* imodel, ModelNode* parent, const struct aiNode* anode) {
    ModelNode* node = new ModelNode;

    node->Name = GetString(anode->mName);
    node->Parent = parent;
    node->LocalTransform = Matrix4x4::Create();
    node->GlobalTransform = Matrix4x4::Create();

    node->TransformMatrix = CopyMatrix(anode->mTransformation);
    Matrix4x4::Copy(node->LocalTransform, node->TransformMatrix);

    node->Children.resize(anode->mNumChildren);
    for (size_t i = 0; i < anode->mNumChildren; i++)
        node->Children[i] = LoadNode(imodel, node, anode->mChildren[i]);

    for (size_t i = 0; i < anode->mNumMeshes; i++) {
        int meshID = MeshIDs[anode->mMeshes[i]];
        if (meshID != -1)
            node->Meshes.push_back(imodel->Meshes[meshID]);
    }

    return node;
}

PRIVATE STATIC Skeleton* ModelImporter::LoadBones(IModel* imodel, Mesh* mesh, struct aiMesh* amesh) {
    Skeleton* skeleton = new Skeleton;

    skeleton->NumBones = amesh->mNumBones;
    skeleton->NumVertices = mesh->VertexCount;
    skeleton->Bones = new MeshBone*[skeleton->NumBones];
    skeleton->VertexWeights = (Uint32*)Memory::Calloc(skeleton->NumVertices, sizeof(Uint32));
    skeleton->PositionBuffer = mesh->PositionBuffer;
    skeleton->NormalBuffer = mesh->NormalBuffer;
    skeleton->GlobalInverseMatrix = imodel->GlobalInverseMatrix;

    for (size_t i = 0; i < skeleton->NumBones; i++) {
        struct aiBone* abone = amesh->mBones[i];

        MeshBone* bone = new MeshBone;
        bone->Name = GetString(abone->mName);
        bone->InverseBindMatrix = CopyMatrix(abone->mOffsetMatrix);

        for (size_t w = 0; w < abone->mNumWeights; w++) {
            struct aiVertexWeight& aweight = abone->mWeights[w];
            Uint32 vertexID = aweight.mVertexId;
            Uint32 weight = aweight.mWeight * 0x10000;
            if (weight != 0) {
                BoneWeight boneWeight;
                boneWeight.VertexID = vertexID;
                boneWeight.Weight = weight;
                bone->Weights.push_back(boneWeight);
            }

            skeleton->VertexWeights[vertexID] += weight;
        }

        // FIXME: Blender's Collada exporter prefixes the Armature name, so this won't work as-is.
        ModelNode* node = imodel->BaseArmature->RootNode->Search(bone->Name);
        if (node)
            bone->GlobalTransform = node->GlobalTransform;
        else
            LogWarn("In mesh %s: Couldn't find node for bone %s", mesh->Name, bone->Name);

        skeleton->Bones[i] = bone;
    }

    return skeleton;
}

PRIVATE STATIC ModelAnim* ModelImporter::LoadAnimation(IModel* imodel, struct aiAnimation* aanim) {
    ModelAnim* anim = new ModelAnim;
    anim->Name = GetString(aanim->mName);
    anim->Channels.resize(aanim->mNumChannels);
    anim->NodeLookup = new HashMap<NodeAnim*>(NULL, 256); // Might be enough

    double baseDuration = ceil(aanim->mDuration + 1.0);
    double ticksPerSecond = aanim->mTicksPerSecond;
    if (ticksPerSecond == 0.0)
        ticksPerSecond = 24.0; // Blender's default

    double durationInSeconds = baseDuration / ticksPerSecond;

    anim->Length = (int)baseDuration;
    anim->DurationInFrames = (int)(durationInSeconds * 60);
    anim->BaseDuration = baseDuration;
    anim->TicksPerSecond = ticksPerSecond;

    for (size_t i = 0; i < aanim->mNumChannels; i++) {
        struct aiNodeAnim* channel = aanim->mChannels[i];
        NodeAnim* nodeAnim = new NodeAnim;

        nodeAnim->NodeName = GetString(channel->mNodeName);
        nodeAnim->PostState = ConvertPrePostState(channel->mPostState);
        nodeAnim->PreState = ConvertPrePostState(channel->mPreState);

        nodeAnim->PositionKeys.resize(channel->mNumPositionKeys);
        nodeAnim->NumPositionKeys = channel->mNumPositionKeys;
        for (size_t j = 0; j < nodeAnim->NumPositionKeys; j++)
            nodeAnim->PositionKeys[j] = GetVectorKey(&channel->mPositionKeys[j]);

        nodeAnim->RotationKeys.resize(channel->mNumRotationKeys);
        nodeAnim->NumRotationKeys = channel->mNumRotationKeys;
        for (size_t j = 0; j < nodeAnim->NumRotationKeys; j++)
            nodeAnim->RotationKeys[j] = GetQuatKey(&channel->mRotationKeys[j]);

        nodeAnim->ScalingKeys.resize(channel->mNumScalingKeys);
        nodeAnim->NumScalingKeys = channel->mNumScalingKeys;
        for (size_t j = 0; j < nodeAnim->NumScalingKeys; j++)
            nodeAnim->ScalingKeys[j] = GetVectorKey(&channel->mScalingKeys[j]);

        anim->NodeLookup->Put(nodeAnim->NodeName, nodeAnim);
        anim->Channels.push_back(nodeAnim);
    }

    return anim;
}

PRIVATE STATIC bool ModelImporter::DoConversion(const struct aiScene* scene, IModel* imodel) {
    if (!scene->mNumMeshes)
        return false;

    size_t meshCount = 0;
    size_t totalVertices = 0;

    vector<struct aiMesh*> ameshes;
    ameshes.clear();

    for (size_t i = 0; i < scene->mNumMeshes; i++) {
        struct aiMesh* amesh = scene->mMeshes[i];
        if (!amesh->HasPositions()) {
            MeshIDs.push_back(-1);
            continue;
        }

        ameshes.push_back(amesh);
        MeshIDs.push_back(meshCount++);

        totalVertices += amesh->mNumVertices;
    }

    // No meshes?
    if (!meshCount)
        return false;

    // Create model
    imodel->Meshes = new Mesh*[meshCount];
    imodel->MeshCount = meshCount;
    imodel->VertexCount = totalVertices;
    imodel->VertexPerFace = 3;

    // Load materials
    if (scene->HasMaterials()) {
        imodel->MaterialCount = scene->mNumMaterials;
        imodel->Materials = new Material*[imodel->MaterialCount];

        for (size_t i = 0; i < imodel->MaterialCount; i++)
            imodel->Materials[i] = LoadMaterial(imodel, scene->mMaterials[i]);
    }

    // Load meshes
    for (size_t i = 0; i < meshCount; i++)
        imodel->Meshes[i] = LoadMesh(imodel, ameshes[i]);

    // Load all nodes, starting from the root
    Armature* armature = new Armature;
    armature->RootNode = LoadNode(imodel, nullptr, scene->mRootNode);

    imodel->BaseArmature = armature;
    imodel->GlobalInverseMatrix = Matrix4x4::Create();

    // Invert the root node's matrix, making a global inverse matrix
    Matrix4x4::Invert(imodel->GlobalInverseMatrix, armature->RootNode->TransformMatrix);

    // Load bones
    vector<Skeleton*> skeletons;

    for (size_t i = 0; i < meshCount; i++) {
        Mesh* mesh = imodel->Meshes[i];

        // There may be less skeletons than meshes, which is normal.
        if (ameshes[i]->HasBones()) {
            Skeleton* skeleton = LoadBones(imodel, mesh, ameshes[i]);

            // To figure out which skeleton number the mesh uses in an armature,
            // we directly store it in the mesh.
            mesh->SkeletonIndex = skeletons.size();

            // Remember this skeleton for storing it in the base armature
            skeletons.push_back(skeleton);
        }
    }

    // Copy all the skeletons that were created, if there were any
    armature->NumSkeletons = skeletons.size();

    if (armature->NumSkeletons) {
        armature->Skeletons = new Skeleton*[armature->NumSkeletons];

        for (size_t i = 0; i < armature->NumSkeletons; i++)
            armature->Skeletons[i] = skeletons[i];
    }

    // Pose and transform the meshes
    imodel->Pose();

    for (size_t i = 0; i < armature->NumSkeletons; i++) {
        Skeleton* skeleton = armature->Skeletons[i];
        skeleton->PrepareTransform();
        skeleton->CalculateBones();
        skeleton->Transform();
    }

    // Load animations
    // FIXME: Doesn't seem to be working with COLLADA scenes for some reason.
    // Might be an issue in assimp?
    if (scene->HasAnimations()) {
        imodel->AnimationCount = scene->mNumAnimations;
        imodel->Animations = new ModelAnim*[imodel->AnimationCount];

        for (size_t i = 0; i < imodel->AnimationCount; i++)
            imodel->Animations[i] = LoadAnimation(imodel, scene->mAnimations[i]);
    }

    return true;
}
#endif

PUBLIC STATIC bool ModelImporter::Convert(IModel* model, Stream* stream, const char* path) {
#ifdef USING_ASSIMP
    size_t size = stream->Length();
    void* data = Memory::Malloc(size);
    if (data)
        stream->ReadBytes(data, size);
    else {
        Log::Print(Log::LOG_ERROR, "Out of memory importing model %s!", path);
        return false;
    }

    Assimp::Importer importer;

    int flags = aiProcessPreset_TargetRealtime_Fast;
    flags |= aiProcess_ConvertToLeftHanded;

    const struct aiScene* scene = importer.ReadFileFromMemory(data, size, flags);
    if (!scene || !scene->mRootNode || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) {
        if (!scene) {
            const char* error = importer.GetErrorString();
            if (error[0])
                LogError("Couldn't import %s: %s", path, error);
            else // No error?
                LogError("Couldn't import %s", path);
        }
        else if (!scene->mRootNode)
            LogError("Couldn't import %s: No root node", path);
        else if (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE)
            LogError("Couldn't import %s: Scene is incomplete", path);

        Memory::Free(data);

        return false;
    }

    MeshIDs.clear();
    ParentDirectory = StringUtils::GetPath(path);

    bool success = DoConversion(scene, model);

    Memory::Free(data);
    Memory::Free(ParentDirectory);

    return success;
#else
    return false;
#endif
}
