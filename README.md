# CRYENGINE Community Edition
CRYENGINE Community Edition is a new version of the engine developed and maintained by the community. It builds on the foundation of CRYENGINE while introducing new features, improvements, and customization options contributed by developers in their free time.
## Installation
- Download the source code for CRYENGINE 5.7
- Continue to build the solution as stated in the readme file shipped alongside 5.7
- After solution is created extract the files from this solutionMake sure that the new files are imported into the solution
  - Locate the projects:EditorCommonSandbox&nbsp;(found in&nbsp;CRYENGINE/Sandbox)
  - Right-click →&nbsp;Properties&nbsp;→&nbsp;Configuration Properties&nbsp;→&nbsp;C/C++&nbsp;→&nbsp;Advanced.In&nbsp;
  - Disable Specific Warnings, add: 4996## Files Required to 
- Import __FlowFlashEnableDynTexNode.cpp__

File Location: Code\CryEngine\CryFlowGraph\FlowSystem\Nodes\FlowFlashEnableDynTexNode.cpp

Import Location:&nbsp;CRYENGINE_Win64\CRYENGINE\CryEngine\CryFlowGraph\Flow System\Nodes
- __D3D_DXC.cpp, D3D_RT.cpp,&nbsp;D3D_Shader.cpp__

File Location: Code\CryEngine\RenderDll\XRenderD3D9\D3D_DXC.cpp, D3D_RT.cpp,&nbsp;D3D_Shader.cpp

Import Location: CRYENGINE_Win64\CRYENGINE\CryEngine\CryRenderD3D12,&nbsp;CryRenderD3D11\Source Files
 - __D3D_DXC.h,&nbsp;D3D_RT.h,&nbsp;D3D_Shader.h__

File Location: Code\CryEngine\RenderDll\XRenderD3D9\D3D_DXC.h,&nbsp;D3D_RT.h,&nbsp;D3D_Shader.h

Import Location:&nbsp;CRYENGINE_Win64\CRYENGINE\CryEngine\CryRenderD3D12,&nbsp;CryRenderD3D11\Header Files
## License&nbsp;
All files provided here are licensed under the MIT License, meaning you are free to use them in your own projects at no cost.The engine itself remains under the CRYENGINE license provided by Crytek. You must comply with the terms of that license when using the engine.Before downloading or using these files, please review the official CRYENGINE license on the CRYENGINE website&nbsp;to ensure you fully understand the requirements.Thank you for respecting both licenses and supporting the community effort.
