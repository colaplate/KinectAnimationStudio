#include "FBXTransmitter.h"
#include "FBXJointConverter.h"

/// <summary>
/// Constructor
/// </summary>
/// <param name="fManager">Pointer to FBX SDK Manager</param>
/// <param name="tPort">Transmitter port, which server binds to, and client connects to</param>
/// <param name="cHostName">Nameof the host for which client will be connected to</param>
FBXTransmitter::FBXTransmitter(FbxManager *fManager, int tPort, char *cHostName) : p_sdkManager(fManager),
p_transmitterPort(tPort),
p_clientHostName(cHostName){

	// Init sockets
	initSockets();

}

/// <summary>
/// Destructor
/// </summary>
FBXTransmitter::~FBXTransmitter() {
	// Destroy host name
	if (p_clientHostName != NULL)
		free(p_clientHostName);


	// Clean up sockets
	WSACleanup();
}
/// <summary>
/// Initialize sockets structure
/// </summary>
void FBXTransmitter::initSockets() {

	// Declare and initialize variables
	WSADATA wsaData;



	// Initialize Winsock
	int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0) {
		return;
	}

	// Initialize host name
	if (p_clientHostName == NULL) {
		char hName[50];
		int hResult = gethostname(
			hName,
			50
			);
		if (hResult == SOCKET_ERROR) {
			p_clientHostName = _strdup("localhost");
		}
		else {
			p_clientHostName = _strdup(hName);
		}
	}

}

/// <summary>
/// Send file to the server
/// </summary>
/// <param name="inputFileName">MoCap file to be sent over the network</param>
void FBXTransmitter::transmit(char *inputFileName) {


	/* initialize random seed: */
	srand(1000);

	// Check if server mode is enabled, if so, do not transmit
	if (p_serverMode) {
		UI_Printf("WARNING: Server mode has been enabled, client mode is disabled.");
		return;
	}

	// Create a scene
	FbxScene* lScene = FbxScene::Create(p_sdkManager, "");
	
	// Load File
	bool r = LoadScene(p_sdkManager, lScene, inputFileName);

	// Check if load was succesful
	if (!r) {
		UI_Printf("	Problem found when trying to load the scene. Make sure %s is a valid FBX file", inputFileName);
		lScene->Destroy();
		return;
	}

	UI_Printf(" File %s has been succesfully loaded.", inputFileName);

	// Get children in the scene
	int childCount = lScene->GetRootNode()->GetChildCount();

	// For each node that is a child of the root
	for (int i = 0; i < childCount; i++) {

		// Get scene child
		FbxNode *pNode = lScene->GetRootNode()->GetChild(i);

		if (pNode == NULL)
			continue;

		FbxNodeAttribute::EType pNodeAttType = pNode->GetNodeAttribute()->GetAttributeType();

		// If not skeleton root, ignore node
		if (pNodeAttType != FbxNodeAttribute::EType::eSkeleton)
			continue;

		// Convert to positional markers ( markers are added to the scene )
		FbxNode *markerSet = FBXJointConverter::toAbsoluteMarkers(lScene, pNode);

		// Apply unroll filter to ir
		FbxAnimCurveFilterUnroll postProcFilter;
		applyUnrollFilterHierarchically(postProcFilter, markerSet);

		FbxAnimStack *animStack = lScene->GetCurrentAnimationStack();
		FbxAnimLayer *animLayer = animStack->GetMember<FbxAnimLayer>();

		FbxAnimCurve *rootCurveX = pNode->LclTranslation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_X);

		std::vector<FbxTime> keyTimeVec;
		FBXJointConverter::extractKeyTimesFromCurve(rootCurveX, keyTimeVec);

		UI_Printf("Ready to drop keys for %s", markerSet->GetName());



		// Back to skeleton
		FbxNode *skel2 = FBXJointConverter::fromAbsoluteMarkers(lScene, pNode, "Bip02", keyTimeVec);


		dropKeys(lScene, skel2);

	}


	// Save Scene
	int lFileFormat = p_sdkManager->GetIOPluginRegistry()->FindReaderIDByDescription(c_FBXBinaryFileDesc);
	
	r = SaveScene(p_sdkManager, lScene, "output.fbx", lFileFormat,false);
	if (!r) {
		UI_Printf(" Problem when trying to save scene.");
	}

	lScene->Destroy();
}

/// <summary>
/// Start Server. Client mode will not work
/// </summary>
void FBXTransmitter::startServer() {

	// Result struct
	struct addrinfo *result = NULL;
	
	// enable flag
	p_serverMode = true;

	SOCKET ListenSocket = createDefaultListeningSocket();
	if (ListenSocket == INVALID_SOCKET) {
		p_serverMode = false;
		return;
	}

	// Incoming client socket
	SOCKET ClientSocket = INVALID_SOCKET;


	// Accept client socket
	ClientSocket = accept(ListenSocket, NULL, NULL);
	if (ClientSocket == INVALID_SOCKET) {
		printf("accept failed with error: %d\n", WSAGetLastError());
		closesocket(ListenSocket);
		WSACleanup();
		return ;
	}


}

/// <summary>
/// Creates a listening socket.
/// </summary>
/// <return>On success, returns copy of socket. On error, it returns an INVALID_SOCKET</return>
SOCKET FBXTransmitter::createDefaultListeningSocket() {
	SOCKET s = INVALID_SOCKET;
	
	
	struct addrinfo *result = NULL,  hints;

	ZeroMemory(&hints, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE;


	char portStr[10];
	sprintf_s(portStr, "%d", p_transmitterPort);
	// Resolve the local address and port to be used by the server
	int iResult = getaddrinfo(NULL, portStr, &hints, &result);
	if (iResult != 0) {
		UI_Printf("Failed to obtain binding address information");
		WSACleanup();
		return s;
	}

	// Create a SOCKET for the server to listen for client connections
	s = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	if (s == INVALID_SOCKET) {
		UI_Printf("Problem while trying to create listening socket. Server will not be started. Error code is %d", WSAGetLastError());
		WSACleanup();
		return s;
	}


	// Bind to socket
	iResult = bind(s, result->ai_addr, (int)result->ai_addrlen);
	if (iResult == SOCKET_ERROR) {
		UI_Printf("Binding to socket failed with error: %d\n", WSAGetLastError());
		freeaddrinfo(result);
		closesocket(s);
		WSACleanup();
		return INVALID_SOCKET;
	}

	iResult = listen(s, SOMAXCONN);
	if (iResult == SOCKET_ERROR) {
		UI_Printf("Listen failed with error: %d\n", WSAGetLastError());
		closesocket(s);
		WSACleanup();
		return INVALID_SOCKET;
	}

	UI_Printf("Listening Socket has been succesfully created at port %d", p_transmitterPort);

	// free result
	freeaddrinfo(result);

	return s;
};

void FBXTransmitter::dropKeys(FbxScene *lScene, FbxNode *curNode) {

	// Lose 60 percent
	int lossRate = 9;


	FbxAnimStack *animStack = lScene->GetCurrentAnimationStack();
	FbxAnimLayer *animLayer = animStack->GetMember<FbxAnimLayer>();


	// Obtain translation curves
	FbxAnimCurve *transXCurve = curNode->LclTranslation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_X);
	FbxAnimCurve *transYCurve = curNode->LclTranslation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_Y);
	FbxAnimCurve *transZCurve = curNode->LclTranslation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_Z);

	int translationKeyCount = 0;
	// Ignore curves that have no or a single key
	if (transXCurve && transXCurve->KeyGetCount() > 1) {
		translationKeyCount = transXCurve->KeyGetCount();
	}

	// Iterate on translation keys
	for (int j = 1; j < translationKeyCount; j++) {
		int random = rand() % 10;
		// Drop packet
	//	UI_Printf("Number %d", random);
		if (random < lossRate) {
	//		UI_Printf("Dropping translation key at index %d for marker %d", j, i);

			if (transXCurve) {
				transXCurve->KeyModifyBegin();
				//Let's drop key from x curve
				transXCurve->KeyRemove(j);
				transXCurve->KeyModifyEnd();
			}

			if (transYCurve) {
				transYCurve->KeyModifyBegin();
				//Let's drop key from y curve
				transYCurve->KeyRemove(j);
				transYCurve->KeyModifyEnd();
			}

			if (transZCurve) {
				transZCurve->KeyModifyBegin();
				//Let's drop key from z curve
				transZCurve->KeyRemove(j);
				transZCurve->KeyModifyEnd();
			}
		}
	}


	// Obtain rotation curves
	FbxAnimCurve *rotXCurve = curNode->LclRotation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_X);
	FbxAnimCurve *rotYCurve = curNode->LclRotation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_Y);
	FbxAnimCurve *rotZCurve = curNode->LclRotation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_Z);

	// Obtain number of keys
	int rotationKeyCount = 0;
	if (rotXCurve) {
		rotationKeyCount = rotXCurve->KeyGetCount();
	}

	// Iterate on rotation keys
	for (int j = 1; j < rotationKeyCount; j++) {
		int random = rand() % 10;
		// Drop packet
	//	UI_Printf("Number %d", random);
		if (random < lossRate) {
//			UI_Printf("Dropping rotation key at index %d for marker %d", j, i);


			if (rotXCurve) {
				rotXCurve->KeyModifyBegin();
				//Let's drop key from x curve
				rotXCurve->KeyRemove(j);
				rotXCurve->KeyModifyEnd();
			}

			if (transYCurve) {
				rotYCurve->KeyModifyBegin();
				//Let's drop key from y curve
				rotYCurve->KeyRemove(j);
				rotYCurve->KeyModifyEnd();
			}

			if (rotZCurve) {
				rotZCurve->KeyModifyBegin();
				//Let's drop key from z curve
				rotZCurve->KeyRemove(j);
				rotZCurve->KeyModifyEnd();
			}
		}
	}



	// Get number of markers
	int childCount = curNode->GetChildCount();

	// Iterate on markers
	for (int i = 0; i < childCount; i++) {

		// Current marker
		FbxNode *childI = curNode->GetChild(i);

		// Repeat for child
		dropKeys(lScene, childI);
	}


}