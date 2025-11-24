# TetrominoThinker
TetrominoThinker is an **AI-driven Tetris engine** that leverages **bitwise board representation**, **heuristic evaluation**, and **recursive lookahead** to determine optimal moves. Designed for efficiency, scalability, and demonstration of advanced AI and game-theory techniques.

## Features
- **Bitwise Board Representation:** Optimized memory and collision detection using bitmasking for fast computation.
- **Heuristic Evaluation:** Configurable weights for height, holes, bumpiness, wells, and lines cleared.
- **Lookahead Search:** Recursive evaluation of upcoming pieces for strategic planning.
- **Transposition Table:** Caching of board states to avoid redundant computations.
- **Piece Generation:** Fair random “bag” system for generating tetromino sequences.
- **Console Visualization:** Converts the bitwise board into a clear visual representation.
- **Configurable Depth:** Adjustable lookahead depth to control AI foresight.

## Getting Started
### Prerequisites
- C++17 or higher
- Standard C++ compiler (GCC, Clang, or MSVC)
- Optional: Windows console with UTF-16 support for proper rendering

