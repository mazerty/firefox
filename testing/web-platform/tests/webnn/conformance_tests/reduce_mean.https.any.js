// META: title=test WebNN API reduction operations
// META: global=window
// META: variant=?cpu
// META: variant=?gpu
// META: variant=?npu
// META: script=../resources/utils.js
// META: timeout=long

'use strict';

// https://www.w3.org/TR/webnn/#dom-mlgraphbuilder-reducemean
// Reduce the input tensor along all dimensions, or along the axes specified in
// the axes array parameter.
//
// dictionary MLReduceOptions {
//   sequence<[EnforceRange] unsigned long> axes;
//   boolean keepDimensions = false;
// };
//
// MLOperand reduceMean(MLOperand input, optional MLReduceOptions options = {});

const reduceMeanTests = [
  {
    'name': 'reduceMean float32 0D constant tensor default options',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [95.84498596191406],
          'descriptor': {shape: [], dataType: 'float32'},
          'constant': true
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [{'input': 'reduceMeanInput'}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': 95.84498596191406,
          'descriptor': {shape: [], dataType: 'float32'}
        }
      }
    }
  },
  {
    'name': 'reduceMean float32 0D constant tensor empty axes',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [95.84498596191406],
          'descriptor': {shape: [], dataType: 'float32'},
          'constant': true
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [{'input': 'reduceMeanInput'}, {'options': {'axes': []}}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': 95.84498596191406,
          'descriptor': {shape: [], dataType: 'float32'}
        }
      }
    }
  },
  {
    'name': 'reduceMean float32 1D constant tensor empty axes',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [-95.84498596191406, 95.84498596191405],
          'descriptor': {shape: [2], dataType: 'float32'},
          'constant': true
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [{'input': 'reduceMeanInput'}, {'options': {'axes': []}}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': [-95.84498596191406, 95.84498596191405],
          'descriptor': {shape: [2], dataType: 'float32'}
        }
      }
    }
  },
  {
    'name':
        'reduceMean float32 1D constant tensor all positive default options',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.84498596191406,  75.6937026977539,   1.5417721271514893,
            8.787034034729004,  70.08280181884766,  13.784331321716309,
            20.006067276000977, 94.80963897705078,  25.82918930053711,
            94.13260650634766,  67.72958374023438,  16.09935188293457,
            92.1943359375,      11.567352294921875, 52.70549774169922,
            22.471792221069336, 3.662332534790039,  20.210277557373047,
            58.56523132324219,  28.673492431640625, 42.13419723510742,
            21.63775062561035,  14.160697937011719, 15.127351760864258
          ],
          'descriptor': {shape: [24], dataType: 'float32'},
          'constant': true
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [{'input': 'reduceMeanInput'}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': 40.31047439575195,
          'descriptor': {shape: [], dataType: 'float32'}
        }
      }
    }
  },
  {
    'name': 'reduceMean float32 1D tensor all positive default options',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.84498596191406,  75.6937026977539,   1.5417721271514893,
            8.787034034729004,  70.08280181884766,  13.784331321716309,
            20.006067276000977, 94.80963897705078,  25.82918930053711,
            94.13260650634766,  67.72958374023438,  16.09935188293457,
            92.1943359375,      11.567352294921875, 52.70549774169922,
            22.471792221069336, 3.662332534790039,  20.210277557373047,
            58.56523132324219,  28.673492431640625, 42.13419723510742,
            21.63775062561035,  14.160697937011719, 15.127351760864258
          ],
          'descriptor': {shape: [24], dataType: 'float32'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [{'input': 'reduceMeanInput'}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': 40.31047439575195,
          'descriptor': {shape: [], dataType: 'float32'}
        }
      }
    }
  },
  {
    'name': 'reduceMean float32 1D tensor all negative default options',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            -37.14686965942383,  -44.500423431396484, -6.1265482902526855,
            -6.321793079376221,  -76.53897857666016,  -4.137692928314209,
            -20.76356315612793,  -38.749176025390625, -36.81039810180664,
            -26.274377822875977, -12.566819190979004, -55.28200912475586,
            -20.69756507873535,  -34.19586181640625,  -45.36003112792969,
            -34.996192932128906, -67.84308624267578,  -0.7434244155883789,
            -21.981258392333984, -61.31269454956055,  -58.598960876464844,
            -76.02980041503906,  -23.91740608215332,  -22.94187355041504
          ],
          'descriptor': {shape: [24], dataType: 'float32'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [{'input': 'reduceMeanInput'}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': -34.74319839477539,
          'descriptor': {shape: [], dataType: 'float32'}
        }
      }
    }
  },
  {
    'name':
        'reduceMean float32 1D tensor all positive integers default options',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            42, 24, 44, 38, 82, 93, 64, 40, 48, 78, 81, 59,
            45, 18, 3,  77, 60, 19, 66, 8,  21, 19, 62, 71
          ],
          'descriptor': {shape: [24], dataType: 'float32'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [{'input': 'reduceMeanInput'}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': 48.41666793823242,
          'descriptor': {shape: [], dataType: 'float32'}
        }
      }
    }
  },
  {
    'name':
        'reduceMean float32 1D tensor all negative integers default options',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            -73, -8,  -55, -73, -61, -54, -5,  -39, -66, -53, -57, -39,
            -62, -98, -36, -1,  -75, -8,  -71, -72, -67, -16, -21, -31
          ],
          'descriptor': {shape: [24], dataType: 'float32'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [{'input': 'reduceMeanInput'}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': -47.54166793823242,
          'descriptor': {shape: [], dataType: 'float32'}
        }
      }
    }
  },
  {
    'name': 'reduceMean float32 2D tensor default options',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.84498596191406,  75.6937026977539,   1.5417721271514893,
            8.787034034729004,  70.08280181884766,  13.784331321716309,
            20.006067276000977, 94.80963897705078,  25.82918930053711,
            94.13260650634766,  67.72958374023438,  16.09935188293457,
            92.1943359375,      11.567352294921875, 52.70549774169922,
            22.471792221069336, 3.662332534790039,  20.210277557373047,
            58.56523132324219,  28.673492431640625, 42.13419723510742,
            21.63775062561035,  14.160697937011719, 15.127351760864258
          ],
          'descriptor': {shape: [4, 6], dataType: 'float32'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [{'input': 'reduceMeanInput'}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': 40.31047439575195,
          'descriptor': {shape: [], dataType: 'float32'}
        }
      }
    }
  },
  {
    'name': 'reduceMean float32 3D tensor default options',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.84498596191406,  75.6937026977539,   1.5417721271514893,
            8.787034034729004,  70.08280181884766,  13.784331321716309,
            20.006067276000977, 94.80963897705078,  25.82918930053711,
            94.13260650634766,  67.72958374023438,  16.09935188293457,
            92.1943359375,      11.567352294921875, 52.70549774169922,
            22.471792221069336, 3.662332534790039,  20.210277557373047,
            58.56523132324219,  28.673492431640625, 42.13419723510742,
            21.63775062561035,  14.160697937011719, 15.127351760864258
          ],
          'descriptor': {shape: [2, 3, 4], dataType: 'float32'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [{'input': 'reduceMeanInput'}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': 40.31047439575195,
          'descriptor': {shape: [], dataType: 'float32'}
        }
      }
    }
  },
  {
    'name': 'reduceMean float32 4D tensor default options',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.84498596191406,  75.6937026977539,   1.5417721271514893,
            8.787034034729004,  70.08280181884766,  13.784331321716309,
            20.006067276000977, 94.80963897705078,  25.82918930053711,
            94.13260650634766,  67.72958374023438,  16.09935188293457,
            92.1943359375,      11.567352294921875, 52.70549774169922,
            22.471792221069336, 3.662332534790039,  20.210277557373047,
            58.56523132324219,  28.673492431640625, 42.13419723510742,
            21.63775062561035,  14.160697937011719, 15.127351760864258
          ],
          'descriptor': {shape: [2, 2, 2, 3], dataType: 'float32'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [{'input': 'reduceMeanInput'}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': 40.31047439575195,
          'descriptor': {shape: [], dataType: 'float32'}
        }
      }
    }
  },
  {
    'name': 'reduceMean float32 5D tensor default options',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.84498596191406,  75.6937026977539,   1.5417721271514893,
            8.787034034729004,  70.08280181884766,  13.784331321716309,
            20.006067276000977, 94.80963897705078,  25.82918930053711,
            94.13260650634766,  67.72958374023438,  16.09935188293457,
            92.1943359375,      11.567352294921875, 52.70549774169922,
            22.471792221069336, 3.662332534790039,  20.210277557373047,
            58.56523132324219,  28.673492431640625, 42.13419723510742,
            21.63775062561035,  14.160697937011719, 15.127351760864258
          ],
          'descriptor': {shape: [2, 1, 4, 1, 3], dataType: 'float32'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [{'input': 'reduceMeanInput'}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': 40.31047439575195,
          'descriptor': {shape: [], dataType: 'float32'}
        }
      }
    }
  },
  {
    'name': 'reduceMean float32 3D tensor options.axes',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.84498596191406,  75.6937026977539,   1.5417721271514893,
            8.787034034729004,  70.08280181884766,  13.784331321716309,
            20.006067276000977, 94.80963897705078,  25.82918930053711,
            94.13260650634766,  67.72958374023438,  16.09935188293457,
            92.1943359375,      11.567352294921875, 52.70549774169922,
            22.471792221069336, 3.662332534790039,  20.210277557373047,
            58.56523132324219,  28.673492431640625, 42.13419723510742,
            21.63775062561035,  14.160697937011719, 15.127351760864258
          ],
          'descriptor': {shape: [2, 3, 4], dataType: 'float32'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [{'input': 'reduceMeanInput'}, {'options': {'axes': [2]}}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': [
            45.46687316894531, 49.670711517333984, 50.94768142700195,
            44.734745025634766, 27.777833938598633, 23.264999389648438
          ],
          'descriptor': {shape: [2, 3], dataType: 'float32'}
        }
      }
    }
  },
  {
    'name': 'reduceMean float32 4D tensor options.axes',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.84498596191406,  75.6937026977539,   1.5417721271514893,
            8.787034034729004,  70.08280181884766,  13.784331321716309,
            20.006067276000977, 94.80963897705078,  25.82918930053711,
            94.13260650634766,  67.72958374023438,  16.09935188293457,
            92.1943359375,      11.567352294921875, 52.70549774169922,
            22.471792221069336, 3.662332534790039,  20.210277557373047,
            58.56523132324219,  28.673492431640625, 42.13419723510742,
            21.63775062561035,  14.160697937011719, 15.127351760864258
          ],
          'descriptor': {shape: [2, 2, 2, 3], dataType: 'float32'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments':
            [{'input': 'reduceMeanInput'}, {'options': {'axes': [0, 2]}}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': [
            54.82453536987305, 40.251548767089844, 22.060470581054688,
            48.58541488647461, 51.343353271484375, 24.797523498535156
          ],
          'descriptor': {shape: [2, 3], dataType: 'float32'}
        }
      }
    }
  },
  {
    'name': 'reduceMean float32 3D tensor options.keepDimensions=false',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.84498596191406,  75.6937026977539,   1.5417721271514893,
            8.787034034729004,  70.08280181884766,  13.784331321716309,
            20.006067276000977, 94.80963897705078,  25.82918930053711,
            94.13260650634766,  67.72958374023438,  16.09935188293457,
            92.1943359375,      11.567352294921875, 52.70549774169922,
            22.471792221069336, 3.662332534790039,  20.210277557373047,
            58.56523132324219,  28.673492431640625, 42.13419723510742,
            21.63775062561035,  14.160697937011719, 15.127351760864258
          ],
          'descriptor': {shape: [2, 3, 4], dataType: 'float32'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [
          {'input': 'reduceMeanInput'}, {'options': {'keepDimensions': false}}
        ],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': 40.31047439575195,
          'descriptor': {shape: [], dataType: 'float32'}
        }
      }
    }
  },
  {
    'name': 'reduceMean float32 3D tensor options.keepDimensions=true',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.84498596191406,  75.6937026977539,   1.5417721271514893,
            8.787034034729004,  70.08280181884766,  13.784331321716309,
            20.006067276000977, 94.80963897705078,  25.82918930053711,
            94.13260650634766,  67.72958374023438,  16.09935188293457,
            92.1943359375,      11.567352294921875, 52.70549774169922,
            22.471792221069336, 3.662332534790039,  20.210277557373047,
            58.56523132324219,  28.673492431640625, 42.13419723510742,
            21.63775062561035,  14.160697937011719, 15.127351760864258
          ],
          'descriptor': {shape: [2, 3, 4], dataType: 'float32'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [
          {'input': 'reduceMeanInput'}, {'options': {'keepDimensions': true}}
        ],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': [40.31047439575195],
          'descriptor': {shape: [1, 1, 1], dataType: 'float32'}
        }
      }
    }
  },
  {
    'name': 'reduceMean float32 4D tensor options.keepDimensions=false',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.84498596191406,  75.6937026977539,   1.5417721271514893,
            8.787034034729004,  70.08280181884766,  13.784331321716309,
            20.006067276000977, 94.80963897705078,  25.82918930053711,
            94.13260650634766,  67.72958374023438,  16.09935188293457,
            92.1943359375,      11.567352294921875, 52.70549774169922,
            22.471792221069336, 3.662332534790039,  20.210277557373047,
            58.56523132324219,  28.673492431640625, 42.13419723510742,
            21.63775062561035,  14.160697937011719, 15.127351760864258
          ],
          'descriptor': {shape: [2, 2, 2, 3], dataType: 'float32'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [
          {'input': 'reduceMeanInput'}, {'options': {'keepDimensions': false}}
        ],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': 40.31047439575195,
          'descriptor': {shape: [], dataType: 'float32'}
        }
      }
    }
  },
  {
    'name': 'reduceMean float32 4D tensor options.keepDimensions=true',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.84498596191406,  75.6937026977539,   1.5417721271514893,
            8.787034034729004,  70.08280181884766,  13.784331321716309,
            20.006067276000977, 94.80963897705078,  25.82918930053711,
            94.13260650634766,  67.72958374023438,  16.09935188293457,
            92.1943359375,      11.567352294921875, 52.70549774169922,
            22.471792221069336, 3.662332534790039,  20.210277557373047,
            58.56523132324219,  28.673492431640625, 42.13419723510742,
            21.63775062561035,  14.160697937011719, 15.127351760864258
          ],
          'descriptor': {shape: [2, 2, 2, 3], dataType: 'float32'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [
          {'input': 'reduceMeanInput'}, {'options': {'keepDimensions': true}}
        ],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': [40.31047439575195],
          'descriptor': {shape: [1, 1, 1, 1], dataType: 'float32'}
        }
      }
    }
  },
  {
    'name':
        'reduceMean float32 4D tensor options.axes with options.keepDimensions=false',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.84498596191406,  75.6937026977539,   1.5417721271514893,
            8.787034034729004,  70.08280181884766,  13.784331321716309,
            20.006067276000977, 94.80963897705078,  25.82918930053711,
            94.13260650634766,  67.72958374023438,  16.09935188293457,
            92.1943359375,      11.567352294921875, 52.70549774169922,
            22.471792221069336, 3.662332534790039,  20.210277557373047,
            58.56523132324219,  28.673492431640625, 42.13419723510742,
            21.63775062561035,  14.160697937011719, 15.127351760864258
          ],
          'descriptor': {shape: [2, 2, 2, 3], dataType: 'float32'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [
          {'input': 'reduceMeanInput'},
          {'options': {'axes': [1, 3], 'keepDimensions': false}}
        ],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': [
            52.287559509277344, 45.10261917114258, 47.640018463134766,
            16.211700439453125
          ],
          'descriptor': {shape: [2, 2], dataType: 'float32'}
        }
      }
    }
  },
  {
    'name':
        'reduceMean float32 4D tensor options.axes with options.keepDimensions=true',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.84498596191406,  75.6937026977539,   1.5417721271514893,
            8.787034034729004,  70.08280181884766,  13.784331321716309,
            20.006067276000977, 94.80963897705078,  25.82918930053711,
            94.13260650634766,  67.72958374023438,  16.09935188293457,
            92.1943359375,      11.567352294921875, 52.70549774169922,
            22.471792221069336, 3.662332534790039,  20.210277557373047,
            58.56523132324219,  28.673492431640625, 42.13419723510742,
            21.63775062561035,  14.160697937011719, 15.127351760864258
          ],
          'descriptor': {shape: [2, 2, 2, 3], dataType: 'float32'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [
          {'input': 'reduceMeanInput'},
          {'options': {'axes': [1, 3], 'keepDimensions': true}}
        ],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': [
            52.287559509277344, 45.10261917114258, 47.640018463134766,
            16.211700439453125
          ],
          'descriptor': {shape: [2, 1, 2, 1], dataType: 'float32'}
        }
      }
    }
  },

  // float16 tests
  {
    'name': 'reduceMean float16 0D constant tensor default options',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [95.875],
          'descriptor': {shape: [], dataType: 'float16'},
          'constant': true
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [{'input': 'reduceMeanInput'}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput':
            {'data': [95.875], 'descriptor': {shape: [], dataType: 'float16'}}
      }
    }
  },
  {
    'name': 'reduceMean float16 0D constant tensor empty axes',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [95.875],
          'descriptor': {shape: [], dataType: 'float16'},
          'constant': true
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [{'input': 'reduceMeanInput'}, {'options': {'axes': []}}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput':
            {'data': [95.875], 'descriptor': {shape: [], dataType: 'float16'}}
      }
    }
  },
  {
    'name':
        'reduceMean float16 1D constant tensor all positive default options',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.875,   75.6875,     1.5419921875, 8.7890625,  70.0625,
            13.78125, 20,          94.8125,      25.828125,  94.125,
            67.75,    16.09375,    92.1875,      11.5703125, 52.71875,
            22.46875, 3.662109375, 20.203125,    58.5625,    28.671875,
            42.125,   21.640625,   14.1640625,   15.125
          ],
          'descriptor': {shape: [24], dataType: 'float16'},
          'constant': true
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [{'input': 'reduceMeanInput'}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput':
            {'data': [40.3125], 'descriptor': {shape: [], dataType: 'float16'}}
      }
    }
  },
  {
    'name': 'reduceMean float16 1D tensor all positive default options',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.875,   75.6875,     1.5419921875, 8.7890625,  70.0625,
            13.78125, 20,          94.8125,      25.828125,  94.125,
            67.75,    16.09375,    92.1875,      11.5703125, 52.71875,
            22.46875, 3.662109375, 20.203125,    58.5625,    28.671875,
            42.125,   21.640625,   14.1640625,   15.125
          ],
          'descriptor': {shape: [24], dataType: 'float16'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [{'input': 'reduceMeanInput'}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput':
            {'data': [40.3125], 'descriptor': {shape: [], dataType: 'float16'}}
      }
    }
  },
  {
    'name': 'reduceMean float16 1D tensor all negative default options',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            -37.15625,   -44.5,      -6.125,         -6.3203125, -76.5625,
            -4.13671875, -20.765625, -38.75,         -36.8125,   -26.28125,
            -12.5703125, -55.28125,  -20.703125,     -34.1875,   -45.375,
            -35,         -67.8125,   -0.74365234375, -21.984375, -61.3125,
            -58.59375,   -76,        -23.921875,     -22.9375
          ],
          'descriptor': {shape: [24], dataType: 'float16'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [{'input': 'reduceMeanInput'}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput':
            {'data': [-34.75], 'descriptor': {shape: [], dataType: 'float16'}}
      }
    }
  },
  {
    'name':
        'reduceMean float16 1D tensor all positive integers default options',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            42, 24, 44, 38, 82, 93, 64, 40, 48, 78, 81, 59,
            45, 18, 3,  77, 60, 19, 66, 8,  21, 19, 62, 71
          ],
          'descriptor': {shape: [24], dataType: 'float16'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [{'input': 'reduceMeanInput'}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput':
            {'data': [48.40625], 'descriptor': {shape: [], dataType: 'float16'}}
      }
    }
  },
  {
    'name':
        'reduceMean float16 1D tensor all negative integers default options',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            -73, -8,  -55, -73, -61, -54, -5,  -39, -66, -53, -57, -39,
            -62, -98, -36, -1,  -75, -8,  -71, -72, -67, -16, -21, -31
          ],
          'descriptor': {shape: [24], dataType: 'float16'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [{'input': 'reduceMeanInput'}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': [-47.53125],
          'descriptor': {shape: [], dataType: 'float16'}
        }
      }
    }
  },
  {
    'name': 'reduceMean float16 2D tensor default options',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.875,   75.6875,     1.5419921875, 8.7890625,  70.0625,
            13.78125, 20,          94.8125,      25.828125,  94.125,
            67.75,    16.09375,    92.1875,      11.5703125, 52.71875,
            22.46875, 3.662109375, 20.203125,    58.5625,    28.671875,
            42.125,   21.640625,   14.1640625,   15.125
          ],
          'descriptor': {shape: [4, 6], dataType: 'float16'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [{'input': 'reduceMeanInput'}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput':
            {'data': [40.3125], 'descriptor': {shape: [], dataType: 'float16'}}
      }
    }
  },
  {
    'name': 'reduceMean float16 3D tensor default options',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.875,   75.6875,     1.5419921875, 8.7890625,  70.0625,
            13.78125, 20,          94.8125,      25.828125,  94.125,
            67.75,    16.09375,    92.1875,      11.5703125, 52.71875,
            22.46875, 3.662109375, 20.203125,    58.5625,    28.671875,
            42.125,   21.640625,   14.1640625,   15.125
          ],
          'descriptor': {shape: [2, 3, 4], dataType: 'float16'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [{'input': 'reduceMeanInput'}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput':
            {'data': [40.3125], 'descriptor': {shape: [], dataType: 'float16'}}
      }
    }
  },
  {
    'name': 'reduceMean float16 4D tensor default options',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.875,   75.6875,     1.5419921875, 8.7890625,  70.0625,
            13.78125, 20,          94.8125,      25.828125,  94.125,
            67.75,    16.09375,    92.1875,      11.5703125, 52.71875,
            22.46875, 3.662109375, 20.203125,    58.5625,    28.671875,
            42.125,   21.640625,   14.1640625,   15.125
          ],
          'descriptor': {shape: [2, 2, 2, 3], dataType: 'float16'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [{'input': 'reduceMeanInput'}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput':
            {'data': [40.3125], 'descriptor': {shape: [], dataType: 'float16'}}
      }
    }
  },
  {
    'name': 'reduceMean float16 5D tensor default options',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.875,   75.6875,     1.5419921875, 8.7890625,  70.0625,
            13.78125, 20,          94.8125,      25.828125,  94.125,
            67.75,    16.09375,    92.1875,      11.5703125, 52.71875,
            22.46875, 3.662109375, 20.203125,    58.5625,    28.671875,
            42.125,   21.640625,   14.1640625,   15.125
          ],
          'descriptor': {shape: [2, 1, 4, 1, 3], dataType: 'float16'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [{'input': 'reduceMeanInput'}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput':
            {'data': [40.3125], 'descriptor': {shape: [], dataType: 'float16'}}
      }
    }
  },
  {
    'name': 'reduceMean float16 3D tensor options.axes',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.875,   75.6875,     1.5419921875, 8.7890625,  70.0625,
            13.78125, 20,          94.8125,      25.828125,  94.125,
            67.75,    16.09375,    92.1875,      11.5703125, 52.71875,
            22.46875, 3.662109375, 20.203125,    58.5625,    28.671875,
            42.125,   21.640625,   14.1640625,   15.125
          ],
          'descriptor': {shape: [2, 3, 4], dataType: 'float16'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [{'input': 'reduceMeanInput'}, {'options': {'axes': [2]}}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': [45.46875, 49.65625, 50.9375, 44.75, 27.78125, 23.265625],
          'descriptor': {shape: [2, 3], dataType: 'float16'}
        }
      }
    }
  },
  {
    'name': 'reduceMean float16 4D tensor options.axes',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.875,   75.6875,     1.5419921875, 8.7890625,  70.0625,
            13.78125, 20,          94.8125,      25.828125,  94.125,
            67.75,    16.09375,    92.1875,      11.5703125, 52.71875,
            22.46875, 3.662109375, 20.203125,    58.5625,    28.671875,
            42.125,   21.640625,   14.1640625,   15.125
          ],
          'descriptor': {shape: [2, 2, 2, 3], dataType: 'float16'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments':
            [{'input': 'reduceMeanInput'}, {'options': {'axes': [0, 2]}}],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': [54.84375, 40.25, 22.0625, 48.59375, 51.34375, 24.796875],
          'descriptor': {shape: [2, 3], dataType: 'float16'}
        }
      }
    }
  },
  {
    'name': 'reduceMean float16 3D tensor options.keepDimensions=false',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.875,   75.6875,     1.5419921875, 8.7890625,  70.0625,
            13.78125, 20,          94.8125,      25.828125,  94.125,
            67.75,    16.09375,    92.1875,      11.5703125, 52.71875,
            22.46875, 3.662109375, 20.203125,    58.5625,    28.671875,
            42.125,   21.640625,   14.1640625,   15.125
          ],
          'descriptor': {shape: [2, 3, 4], dataType: 'float16'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [
          {'input': 'reduceMeanInput'}, {'options': {'keepDimensions': false}}
        ],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput':
            {'data': [40.3125], 'descriptor': {shape: [], dataType: 'float16'}}
      }
    }
  },
  {
    'name': 'reduceMean float16 3D tensor options.keepDimensions=true',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.875,   75.6875,     1.5419921875, 8.7890625,  70.0625,
            13.78125, 20,          94.8125,      25.828125,  94.125,
            67.75,    16.09375,    92.1875,      11.5703125, 52.71875,
            22.46875, 3.662109375, 20.203125,    58.5625,    28.671875,
            42.125,   21.640625,   14.1640625,   15.125
          ],
          'descriptor': {shape: [2, 3, 4], dataType: 'float16'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [
          {'input': 'reduceMeanInput'}, {'options': {'keepDimensions': true}}
        ],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': [40.3125],
          'descriptor': {shape: [1, 1, 1], dataType: 'float16'}
        }
      }
    }
  },
  {
    'name': 'reduceMean float16 4D tensor options.keepDimensions=false',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.875,   75.6875,     1.5419921875, 8.7890625,  70.0625,
            13.78125, 20,          94.8125,      25.828125,  94.125,
            67.75,    16.09375,    92.1875,      11.5703125, 52.71875,
            22.46875, 3.662109375, 20.203125,    58.5625,    28.671875,
            42.125,   21.640625,   14.1640625,   15.125
          ],
          'descriptor': {shape: [2, 2, 2, 3], dataType: 'float16'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [
          {'input': 'reduceMeanInput'}, {'options': {'keepDimensions': false}}
        ],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput':
            {'data': [40.3125], 'descriptor': {shape: [], dataType: 'float16'}}
      }
    }
  },
  {
    'name': 'reduceMean float16 4D tensor options.keepDimensions=true',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.875,   75.6875,     1.5419921875, 8.7890625,  70.0625,
            13.78125, 20,          94.8125,      25.828125,  94.125,
            67.75,    16.09375,    92.1875,      11.5703125, 52.71875,
            22.46875, 3.662109375, 20.203125,    58.5625,    28.671875,
            42.125,   21.640625,   14.1640625,   15.125
          ],
          'descriptor': {shape: [2, 2, 2, 3], dataType: 'float16'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [
          {'input': 'reduceMeanInput'}, {'options': {'keepDimensions': true}}
        ],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': [40.3125],
          'descriptor': {shape: [1, 1, 1, 1], dataType: 'float16'}
        }
      }
    }
  },
  {
    'name':
        'reduceMean float16 4D tensor options.axes with options.keepDimensions=false',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.875,   75.6875,     1.5419921875, 8.7890625,  70.0625,
            13.78125, 20,          94.8125,      25.828125,  94.125,
            67.75,    16.09375,    92.1875,      11.5703125, 52.71875,
            22.46875, 3.662109375, 20.203125,    58.5625,    28.671875,
            42.125,   21.640625,   14.1640625,   15.125
          ],
          'descriptor': {shape: [2, 2, 2, 3], dataType: 'float16'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [
          {'input': 'reduceMeanInput'},
          {'options': {'axes': [1, 3], 'keepDimensions': false}}
        ],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': [52.28125, 45.09375, 47.625, 16.203125],
          'descriptor': {shape: [2, 2], dataType: 'float16'}
        }
      }
    }
  },
  {
    'name':
        'reduceMean float16 4D tensor options.axes with options.keepDimensions=true',
    'graph': {
      'inputs': {
        'reduceMeanInput': {
          'data': [
            95.875,   75.6875,     1.5419921875, 8.7890625,  70.0625,
            13.78125, 20,          94.8125,      25.828125,  94.125,
            67.75,    16.09375,    92.1875,      11.5703125, 52.71875,
            22.46875, 3.662109375, 20.203125,    58.5625,    28.671875,
            42.125,   21.640625,   14.1640625,   15.125
          ],
          'descriptor': {shape: [2, 2, 2, 3], dataType: 'float16'}
        }
      },
      'operators': [{
        'name': 'reduceMean',
        'arguments': [
          {'input': 'reduceMeanInput'},
          {'options': {'axes': [1, 3], 'keepDimensions': true}}
        ],
        'outputs': 'reduceMeanOutput'
      }],
      'expectedOutputs': {
        'reduceMeanOutput': {
          'data': [52.28125, 45.09375, 47.625, 16.203125],
          'descriptor': {shape: [2, 1, 2, 1], dataType: 'float16'}
        }
      }
    }
  }
];

if (navigator.ml) {
  reduceMeanTests.filter(isTargetTest).forEach((test) => {
    webnn_conformance_test(buildAndExecuteGraph, getPrecisionTolerance, test);
  });
} else {
  test(() => assert_implements(navigator.ml, 'missing navigator.ml'));
}
