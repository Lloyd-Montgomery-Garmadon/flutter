// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:convert';
import 'dart:io';
import 'dart:isolate';
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/rendering.dart';
import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';

const String _fontFamily = 'VenusMvpRoboto';
const String _locale = 'en_US';
const int _requestHeaderSize = 48;
const int _requestItemStride = 160;
const int _resultHeaderSize = 48;
const int _resultItemStride = 112;
const int _schemaVersion = 1;
const int _noErrorOffset = 0xffffffff;
const int _coreBatchId = 0x0102030405060708;

abstract final class _BatchStatus {
  static const int ok = 0;
  static const int invalidHeader = 1;
  static const int unsupportedVersion = 2;
  static const int invalidTable = 3;
  static const int maxValue = 7;
}

abstract final class _BatchErrorDetail {
  static const int none = 0;
  static const int badMagic = 2;
  static const int badVersion = 3;
  static const int badHeaderSize = 4;
  static const int badByteSize = 5;
  static const int nonzeroFlags = 6;
  static const int badItemStride = 8;
  static const int itemsOffsetAlignment = 9;
  static const int payloadOffsetAlignment = 10;
  static const int nonzeroReserved = 15;
  static const int maxValue = 18;
}

abstract final class _ItemStatus {
  static const int ok = 0;
  static const int invalidEnum = 2;
  static const int duplicateItemId = 7;
  static const int maxValue = 8;
}

abstract final class _ErrorFieldId {
  static const int none = 0;
  static const int textDirection = 20;
  static const int duplicateId = 34;
  static const int maxValue = 39;
}

abstract final class _ItemErrorDetail {
  static const int none = 0;
  static const int aboveMax = 2;
  static const int duplicateLaterOccurrence = 10;
  static const int maxValue = 14;
}

enum _WireTextDirection {
  ltr(1, TextDirection.ltr),
  rtl(2, TextDirection.rtl);

  const _WireTextDirection(this.value, this.flutterValue);
  final int value;
  final TextDirection flutterValue;
}

enum _WireTextAlign {
  left(1, TextAlign.left),
  right(2, TextAlign.right),
  center(3, TextAlign.center),
  justify(4, TextAlign.justify),
  start(5, TextAlign.start),
  end(6, TextAlign.end);

  const _WireTextAlign(this.value, this.flutterValue);
  final int value;
  final TextAlign flutterValue;
}

enum _WireFontStyle {
  normal(1, FontStyle.normal),
  italic(2, FontStyle.italic);

  const _WireFontStyle(this.value, this.flutterValue);
  final int value;
  final FontStyle flutterValue;
}

enum _WireLeadingDistribution {
  proportional(1, TextLeadingDistribution.proportional),
  even(2, TextLeadingDistribution.even);

  const _WireLeadingDistribution(this.value, this.flutterValue);
  final int value;
  final TextLeadingDistribution flutterValue;
}

enum _WireTextWidthBasis {
  parent(1, TextWidthBasis.parent),
  longestLine(2, TextWidthBasis.longestLine);

  const _WireTextWidthBasis(this.value, this.flutterValue);
  final int value;
  final TextWidthBasis flutterValue;
}

enum _WireTextScalerKind {
  noScaling(1),
  linear(2);

  const _WireTextScalerKind(this.value);
  final int value;
}

enum _WireMaxLinesMode {
  unlimited(1),
  bounded(2);

  const _WireMaxLinesMode(this.value);
  final int value;
}

enum _WireOverflowMode {
  clip(1, TextOverflow.clip),
  ellipsis(2, TextOverflow.ellipsis);

  const _WireOverflowMode(this.value, this.flutterValue);
  final int value;
  final TextOverflow flutterValue;
}

enum _WireBool {
  falseValue(1, false),
  trueValue(2, true);

  const _WireBool(this.value, this.flutterValue);
  final int value;
  final bool flutterValue;
}

enum _WireHeightMode {
  fontMetrics(1),
  multiplier(2);

  const _WireHeightMode(this.value);
  final int value;
}

final class _TextRequest {
  const _TextRequest({
    required this.itemId,
    required this.text,
    required this.constraints,
    this.fontFamily = _fontFamily,
    this.locale = _locale,
    this.ellipsis = '\u2026',
    this.fontSize = 17.0,
    this.letterSpacing = 0.3,
    this.wordSpacing = 0.0,
    this.height = 1.4,
    this.scalerParameter = 1.0,
    this.fontWeight = 400,
    this.maxLines = 3,
    this.textDirection = _WireTextDirection.ltr,
    this.textAlign = _WireTextAlign.start,
    this.fontStyle = _WireFontStyle.normal,
    this.leadingDistribution = _WireLeadingDistribution.even,
    this.textWidthBasis = _WireTextWidthBasis.longestLine,
    this.textScalerKind = _WireTextScalerKind.noScaling,
    this.maxLinesMode = _WireMaxLinesMode.bounded,
    this.overflowMode = _WireOverflowMode.ellipsis,
    this.softWrap = _WireBool.trueValue,
    this.applyHeightToFirstAscent = _WireBool.trueValue,
    this.applyHeightToLastDescent = _WireBool.trueValue,
    this.heightMode = _WireHeightMode.multiplier,
  });

  final int itemId;
  final String text;
  final String fontFamily;
  final String locale;
  final String ellipsis;
  final BoxConstraints constraints;
  final double fontSize;
  final double letterSpacing;
  final double wordSpacing;
  final double height;
  final double scalerParameter;
  final int fontWeight;
  final int maxLines;
  final _WireTextDirection textDirection;
  final _WireTextAlign textAlign;
  final _WireFontStyle fontStyle;
  final _WireLeadingDistribution leadingDistribution;
  final _WireTextWidthBasis textWidthBasis;
  final _WireTextScalerKind textScalerKind;
  final _WireMaxLinesMode maxLinesMode;
  final _WireOverflowMode overflowMode;
  final _WireBool softWrap;
  final _WireBool applyHeightToFirstAscent;
  final _WireBool applyHeightToLastDescent;
  final _WireHeightMode heightMode;
}

final class _PayloadRef {
  const _PayloadRef(this.offset, this.length);
  final int offset;
  final int length;
}

final class _PayloadBuilder {
  final BytesBuilder _bytes = BytesBuilder(copy: false);
  final Map<String, _PayloadRef> _shared = <String, _PayloadRef>{};

  _PayloadRef add(String value) {
    return _shared.putIfAbsent(value, () {
      final List<int> encoded = utf8.encode(value);
      final _PayloadRef result = _PayloadRef(_bytes.length, encoded.length);
      _bytes.add(encoded);
      return result;
    });
  }

  Uint8List takeBytes() => _bytes.takeBytes();
}

final class _RequestCodec {
  static Uint8List encode(List<_TextRequest> items, {required int batchId}) {
    final _PayloadBuilder payloadBuilder = _PayloadBuilder();
    final List<_PayloadRef> textRefs = <_PayloadRef>[];
    final List<_PayloadRef> familyRefs = <_PayloadRef>[];
    final List<_PayloadRef> localeRefs = <_PayloadRef>[];
    final List<_PayloadRef?> ellipsisRefs = <_PayloadRef?>[];
    for (final _TextRequest item in items) {
      textRefs.add(payloadBuilder.add(item.text));
      familyRefs.add(payloadBuilder.add(item.fontFamily));
      localeRefs.add(payloadBuilder.add(item.locale));
      ellipsisRefs.add(
        item.overflowMode == _WireOverflowMode.clip ? null : payloadBuilder.add(item.ellipsis),
      );
    }

    final Uint8List payload = payloadBuilder.takeBytes();
    final int itemsOffset = _requestHeaderSize;
    final int payloadOffset = itemsOffset + items.length * _requestItemStride;
    final int byteSize = payloadOffset + payload.length;
    final Uint8List result = Uint8List(byteSize);
    final ByteData data = ByteData.sublistView(result);

    result.setRange(0, 4, ascii.encode('VTLB'));
    data.setUint16(4, _schemaVersion, Endian.little);
    data.setUint16(6, _requestHeaderSize, Endian.little);
    data.setUint32(8, byteSize, Endian.little);
    data.setUint32(12, 0, Endian.little);
    data.setUint32(16, items.length, Endian.little);
    data.setUint32(20, _requestItemStride, Endian.little);
    data.setUint32(24, itemsOffset, Endian.little);
    data.setUint32(28, payloadOffset, Endian.little);
    data.setUint32(32, payload.length, Endian.little);
    data.setUint32(36, 0, Endian.little);
    data.setUint64(40, batchId, Endian.little);

    for (int index = 0; index < items.length; index += 1) {
      final _TextRequest item = items[index];
      final int base = itemsOffset + index * _requestItemStride;
      data.setUint32(base, _requestItemStride, Endian.little);
      data.setUint32(base + 4, 0, Endian.little);
      data.setUint64(base + 8, item.itemId, Endian.little);
      _writeRef(data, base + 16, textRefs[index], payloadOffset);
      _writeRef(data, base + 24, familyRefs[index], payloadOffset);
      _writeRef(data, base + 32, localeRefs[index], payloadOffset);
      final _PayloadRef? ellipsisRef = ellipsisRefs[index];
      if (ellipsisRef != null) {
        _writeRef(data, base + 40, ellipsisRef, payloadOffset);
      }
      data.setFloat64(base + 48, item.constraints.minWidth, Endian.little);
      data.setFloat64(base + 56, item.constraints.maxWidth, Endian.little);
      data.setFloat64(base + 64, item.constraints.minHeight, Endian.little);
      data.setFloat64(base + 72, item.constraints.maxHeight, Endian.little);
      data.setFloat64(base + 80, item.fontSize, Endian.little);
      data.setFloat64(base + 88, item.letterSpacing, Endian.little);
      data.setFloat64(base + 96, item.wordSpacing, Endian.little);
      data.setFloat64(base + 104, item.height, Endian.little);
      data.setFloat64(base + 112, item.scalerParameter, Endian.little);
      data.setUint32(base + 120, item.fontWeight, Endian.little);
      data.setUint32(base + 124, item.maxLines, Endian.little);
      data.setUint16(base + 128, 1, Endian.little);
      data.setUint16(base + 130, item.textDirection.value, Endian.little);
      data.setUint16(base + 132, item.textAlign.value, Endian.little);
      data.setUint16(base + 134, item.fontStyle.value, Endian.little);
      data.setUint16(base + 136, item.leadingDistribution.value, Endian.little);
      data.setUint16(base + 138, item.textWidthBasis.value, Endian.little);
      data.setUint16(base + 140, item.textScalerKind.value, Endian.little);
      data.setUint16(base + 142, item.maxLinesMode.value, Endian.little);
      data.setUint16(base + 144, item.overflowMode.value, Endian.little);
      data.setUint16(base + 146, item.softWrap.value, Endian.little);
      data.setUint16(base + 148, item.applyHeightToFirstAscent.value, Endian.little);
      data.setUint16(base + 150, item.applyHeightToLastDescent.value, Endian.little);
      data.setUint16(base + 152, item.heightMode.value, Endian.little);
      data.setUint16(base + 154, 1, Endian.little);
      data.setUint32(base + 156, 0, Endian.little);
    }
    result.setRange(payloadOffset, byteSize, payload);
    return result;
  }

  static void _writeRef(ByteData data, int offset, _PayloadRef ref, int payloadOffset) {
    data.setUint32(offset, payloadOffset + ref.offset, Endian.little);
    data.setUint32(offset + 4, ref.length, Endian.little);
  }
}

final class _ResultItem {
  const _ResultItem({
    required this.status,
    required this.itemId,
    required this.painterWidthBits,
    required this.painterHeightBits,
    required this.boxWidthBits,
    required this.boxHeightBits,
    required this.minIntrinsicWidthBits,
    required this.maxIntrinsicWidthBits,
    required this.alphabeticBaselineBits,
    required this.ideographicBaselineBits,
    required this.firstLineLeftBits,
    required this.lineCount,
    required this.didExceedMaxLines,
    required this.errorFieldId,
    required this.errorDetail,
  });

  final int status;
  final int itemId;
  final int painterWidthBits;
  final int painterHeightBits;
  final int boxWidthBits;
  final int boxHeightBits;
  final int minIntrinsicWidthBits;
  final int maxIntrinsicWidthBits;
  final int alphabeticBaselineBits;
  final int ideographicBaselineBits;
  final int firstLineLeftBits;
  final int lineCount;
  final bool didExceedMaxLines;
  final int errorFieldId;
  final int errorDetail;
}

final class _BatchResult {
  const _BatchResult({
    required this.status,
    required this.firstErrorOffset,
    required this.errorDetail,
    required this.batchId,
    required this.items,
  });

  final int status;
  final int firstErrorOffset;
  final int errorDetail;
  final int batchId;
  final List<_ResultItem> items;

  static _BatchResult decode(Uint8List bytes) {
    if (bytes.length < _resultHeaderSize) {
      throw const FormatException('Venus result is shorter than its fixed header.');
    }
    final ByteData data = ByteData.sublistView(bytes);
    if (ascii.decode(bytes.sublist(0, 4), allowInvalid: true) != 'VTLR') {
      throw const FormatException('Venus result has an invalid magic.');
    }
    final int version = data.getUint16(4, Endian.little);
    final int headerSize = data.getUint16(6, Endian.little);
    final int byteSize = data.getUint32(8, Endian.little);
    final int status = data.getUint32(12, Endian.little);
    final int itemCount = data.getUint32(16, Endian.little);
    final int itemStride = data.getUint32(20, Endian.little);
    final int itemsOffset = data.getUint32(24, Endian.little);
    final int firstErrorOffset = data.getUint32(28, Endian.little);
    final int errorDetail = data.getUint32(32, Endian.little);
    final int reserved = data.getUint32(36, Endian.little);
    final int batchId = data.getUint64(40, Endian.little);
    if (version != _schemaVersion ||
        headerSize != _resultHeaderSize ||
        byteSize != bytes.length ||
        itemStride != _resultItemStride ||
        itemsOffset != _resultHeaderSize ||
        reserved != 0 ||
        status > _BatchStatus.maxValue ||
        errorDetail > _BatchErrorDetail.maxValue) {
      throw const FormatException('Venus result header is not canonical V1.');
    }
    if (status != _BatchStatus.ok) {
      if (bytes.length != _resultHeaderSize ||
          itemCount != 0 ||
          errorDetail == _BatchErrorDetail.none) {
        throw const FormatException('Venus global failure envelope is not canonical.');
      }
      return _BatchResult(
        status: status,
        firstErrorOffset: firstErrorOffset,
        errorDetail: errorDetail,
        batchId: batchId,
        items: const <_ResultItem>[],
      );
    }
    if (byteSize != _resultHeaderSize + itemCount * _resultItemStride ||
        firstErrorOffset != _noErrorOffset ||
        errorDetail != _BatchErrorDetail.none) {
      throw const FormatException('Venus success envelope is not canonical.');
    }

    final List<_ResultItem> items = <_ResultItem>[];
    for (int index = 0; index < itemCount; index += 1) {
      final int base = itemsOffset + index * itemStride;
      final int structSize = data.getUint32(base, Endian.little);
      final int itemStatus = data.getUint32(base + 4, Endian.little);
      final int didExceedRaw = data.getUint32(base + 92, Endian.little);
      final int flags = data.getUint32(base + 96, Endian.little);
      final int errorFieldId = data.getUint32(base + 100, Endian.little);
      final int itemErrorDetail = data.getUint32(base + 104, Endian.little);
      final int itemReserved = data.getUint32(base + 108, Endian.little);
      if (structSize != _resultItemStride ||
          itemStatus > _ItemStatus.maxValue ||
          didExceedRaw > 1 ||
          flags != 0 ||
          errorFieldId > _ErrorFieldId.maxValue ||
          itemErrorDetail > _ItemErrorDetail.maxValue ||
          itemReserved != 0) {
        throw FormatException('Venus result item $index is not canonical V1.');
      }
      final List<int> geometryBits = <int>[
        for (int offset = 16; offset <= 80; offset += 8)
          data.getUint64(base + offset, Endian.little),
      ];
      if (itemStatus == _ItemStatus.ok) {
        if (errorFieldId != _ErrorFieldId.none || itemErrorDetail != _ItemErrorDetail.none) {
          throw FormatException('Venus success item $index contains an error.');
        }
      } else if (errorFieldId == _ErrorFieldId.none ||
          itemErrorDetail == _ItemErrorDetail.none ||
          geometryBits.any((int bits) => bits != 0) ||
          data.getUint32(base + 88, Endian.little) != 0 ||
          didExceedRaw != 0) {
        throw FormatException('Venus failure item $index is not canonical zero.');
      }
      items.add(
        _ResultItem(
          status: itemStatus,
          itemId: data.getUint64(base + 8, Endian.little),
          painterWidthBits: geometryBits[0],
          painterHeightBits: geometryBits[1],
          boxWidthBits: geometryBits[2],
          boxHeightBits: geometryBits[3],
          minIntrinsicWidthBits: geometryBits[4],
          maxIntrinsicWidthBits: geometryBits[5],
          alphabeticBaselineBits: geometryBits[6],
          ideographicBaselineBits: geometryBits[7],
          firstLineLeftBits: geometryBits[8],
          lineCount: data.getUint32(base + 88, Endian.little),
          didExceedMaxLines: didExceedRaw == 1,
          errorFieldId: errorFieldId,
          errorDetail: itemErrorDetail,
        ),
      );
    }
    return _BatchResult(
      status: status,
      firstErrorOffset: firstErrorOffset,
      errorDetail: errorDetail,
      batchId: batchId,
      items: items,
    );
  }
}

enum _Coverage { wrappedLoose, maxHeightClamp, minWidthClamp, fieldMatrix }

final class _Case {
  const _Case(this.name, this.request, this.coverage);
  final String name;
  final _TextRequest request;
  final _Coverage coverage;
}

final class _FlutterMetrics {
  const _FlutterMetrics({
    required this.painterWidth,
    required this.painterHeight,
    required this.boxWidth,
    required this.boxHeight,
    required this.minIntrinsicWidth,
    required this.maxIntrinsicWidth,
    required this.alphabeticBaseline,
    required this.ideographicBaseline,
    required this.firstLineLeft,
    required this.lineCount,
    required this.didExceedMaxLines,
  });

  final double painterWidth;
  final double painterHeight;
  final double boxWidth;
  final double boxHeight;
  final double minIntrinsicWidth;
  final double maxIntrinsicWidth;
  final double alphabeticBaseline;
  final double ideographicBaseline;
  final double firstLineLeft;
  final int lineCount;
  final bool didExceedMaxLines;
}

int _float64Bits(double value) {
  final ByteData data = ByteData(8)..setFloat64(0, value, Endian.little);
  return data.getUint64(0, Endian.little);
}

double _float64FromBits(int bits) {
  final ByteData data = ByteData(8)..setUint64(0, bits, Endian.little);
  return data.getFloat64(0, Endian.little);
}

String _bitsHex(int bits) => '0x${bits.toRadixString(16).padLeft(16, '0')}';

FontWeight _fontWeight(int value) {
  return FontWeight.values.singleWhere((FontWeight weight) => weight.value == value);
}

Locale _flutterLocale(String value) {
  final List<String> parts = value.split('_');
  return Locale(parts[0], parts[1]);
}

TextScaler _textScaler(_TextRequest request) {
  return switch (request.textScalerKind) {
    _WireTextScalerKind.noScaling => TextScaler.noScaling,
    _WireTextScalerKind.linear => TextScaler.linear(request.scalerParameter),
  };
}

TextStyle _textStyle(_TextRequest request) {
  return TextStyle(
    fontFamily: request.fontFamily,
    fontSize: request.fontSize,
    fontWeight: _fontWeight(request.fontWeight),
    fontStyle: request.fontStyle.flutterValue,
    letterSpacing: request.letterSpacing,
    wordSpacing: request.wordSpacing,
    height: request.heightMode == _WireHeightMode.multiplier ? request.height : null,
    leadingDistribution: request.leadingDistribution.flutterValue,
    locale: _flutterLocale(request.locale),
  );
}

TextHeightBehavior _textHeightBehavior(_TextRequest request) {
  return TextHeightBehavior(
    applyHeightToFirstAscent: request.applyHeightToFirstAscent.flutterValue,
    applyHeightToLastDescent: request.applyHeightToLastDescent.flutterValue,
    leadingDistribution: request.leadingDistribution.flutterValue,
  );
}

Future<_FlutterMetrics> _measureFlutter(WidgetTester tester, _Case testCase) async {
  final _TextRequest request = testCase.request;
  final BoxConstraints constraints = request.constraints;
  expect(
    request.softWrap,
    _WireBool.trueValue,
    reason: 'Phase 1 rejects softWrap=false before paragraph layout.',
  );
  expect(constraints.isTight, isFalse, reason: '${testCase.name} must remain non-tight');
  expect(constraints.hasBoundedWidth, isTrue);
  expect(constraints.hasBoundedHeight, isTrue);
  expect(constraints.minWidth, lessThan(constraints.maxWidth));
  expect(constraints.minHeight, lessThan(constraints.maxHeight));

  final TextScaler scaler = _textScaler(request);
  final TextStyle style = _textStyle(request);
  final TextHeightBehavior heightBehavior = _textHeightBehavior(request);
  final Locale locale = _flutterLocale(request.locale);
  final TextPainter painter = TextPainter(
    text: TextSpan(text: request.text, style: style),
    textAlign: request.textAlign.flutterValue,
    textDirection: request.textDirection.flutterValue,
    textScaler: scaler,
    maxLines: request.maxLinesMode == _WireMaxLinesMode.unlimited ? null : request.maxLines,
    ellipsis: request.overflowMode == _WireOverflowMode.ellipsis ? request.ellipsis : null,
    locale: locale,
    textWidthBasis: request.textWidthBasis.flutterValue,
    textHeightBehavior: heightBehavior,
  )..layout(minWidth: constraints.minWidth, maxWidth: constraints.maxWidth);
  addTearDown(painter.dispose);

  final GlobalKey paragraphKey = GlobalKey();
  await tester.pumpWidget(
    Directionality(
      textDirection: request.textDirection.flutterValue,
      child: MediaQuery(
        data: MediaQueryData(devicePixelRatio: 1.0, textScaler: scaler),
        child: Center(
          child: ConstrainedBox(
            constraints: constraints,
            child: RichText(
              key: paragraphKey,
              text: TextSpan(text: request.text, style: style),
              textAlign: request.textAlign.flutterValue,
              textDirection: request.textDirection.flutterValue,
              softWrap: request.softWrap.flutterValue,
              overflow: request.overflowMode.flutterValue,
              textScaler: scaler,
              maxLines: request.maxLinesMode == _WireMaxLinesMode.unlimited
                  ? null
                  : request.maxLines,
              locale: locale,
              textWidthBasis: request.textWidthBasis.flutterValue,
              textHeightBehavior: heightBehavior,
            ),
          ),
        ),
      ),
    ),
  );
  final RenderParagraph paragraph =
      paragraphKey.currentContext!.findRenderObject()! as RenderParagraph;
  expect(paragraph.constraints, constraints);
  final List<LineMetrics> lines = painter.computeLineMetrics();
  return _FlutterMetrics(
    painterWidth: painter.width,
    painterHeight: painter.height,
    boxWidth: paragraph.size.width,
    boxHeight: paragraph.size.height,
    minIntrinsicWidth: painter.minIntrinsicWidth,
    maxIntrinsicWidth: painter.maxIntrinsicWidth,
    alphabeticBaseline: painter.computeDistanceToActualBaseline(TextBaseline.alphabetic),
    ideographicBaseline: painter.computeDistanceToActualBaseline(TextBaseline.ideographic),
    firstLineLeft: lines.first.left,
    lineCount: lines.length,
    didExceedMaxLines: painter.didExceedMaxLines,
  );
}

void _expectSameBits(int cppBits, double flutter, String metric, String caseName) {
  final int flutterBits = _float64Bits(flutter);
  expect(
    cppBits,
    flutterBits,
    reason:
        '$caseName $metric differs: C++=${_float64FromBits(cppBits)} '
        '(${_bitsHex(cppBits)}) vs Flutter=$flutter (${_bitsHex(flutterBits)})',
  );
}

void _printMetric(String caseName, String metric, int cppBits, double flutter) {
  debugPrint(
    'VENUS_PHASE1_METRIC case="$caseName" metric=$metric '
    'cpp=${_float64FromBits(cppBits)} cpp_bits=${_bitsHex(cppBits)} '
    'flutter=$flutter flutter_bits=${_bitsHex(_float64Bits(flutter))}',
    wrapWidth: 1024,
  );
}

void _expectResultMatches(_ResultItem cpp, _FlutterMetrics flutter, _Case testCase) {
  expect(cpp.status, _ItemStatus.ok, reason: testCase.name);
  expect(cpp.itemId, testCase.request.itemId, reason: testCase.name);
  final List<(String, int, double)> doubles = <(String, int, double)>[
    ('TextPainter.width', cpp.painterWidthBits, flutter.painterWidth),
    ('TextPainter.height', cpp.painterHeightBits, flutter.painterHeight),
    ('RenderParagraph.width', cpp.boxWidthBits, flutter.boxWidth),
    ('RenderParagraph.height', cpp.boxHeightBits, flutter.boxHeight),
    ('minIntrinsicWidth', cpp.minIntrinsicWidthBits, flutter.minIntrinsicWidth),
    ('maxIntrinsicWidth', cpp.maxIntrinsicWidthBits, flutter.maxIntrinsicWidth),
    ('alphabeticBaseline', cpp.alphabeticBaselineBits, flutter.alphabeticBaseline),
    ('ideographicBaseline', cpp.ideographicBaselineBits, flutter.ideographicBaseline),
    ('firstLineLeft', cpp.firstLineLeftBits, flutter.firstLineLeft),
  ];
  for (final (String metric, int cppBits, double flutterValue) in doubles) {
    _printMetric(testCase.name, metric, cppBits, flutterValue);
    _expectSameBits(cppBits, flutterValue, metric, testCase.name);
  }
  expect(cpp.lineCount, flutter.lineCount, reason: '${testCase.name} line count');
  expect(
    cpp.didExceedMaxLines,
    flutter.didExceedMaxLines,
    reason: '${testCase.name} didExceedMaxLines',
  );
}

const List<_Case> _coreCases = <_Case>[
  _Case(
    'wrapped loose box',
    _TextRequest(
      itemId: 0x1001,
      text: 'Venus drives Flutter text shaping across several wrapped words.',
      constraints: BoxConstraints(
        minWidth: 0.0,
        maxWidth: 137.75,
        minHeight: 0.0,
        maxHeight: 90.25,
      ),
    ),
    _Coverage.wrappedLoose,
  ),
  _Case(
    'height clamped by box',
    _TextRequest(
      itemId: 0x1002,
      text: 'Three visible lines are taller than this deliberately short constrained box.',
      constraints: BoxConstraints(
        minWidth: 20.25,
        maxWidth: 137.75,
        minHeight: 18.5,
        maxHeight: 42.25,
      ),
    ),
    _Coverage.maxHeightClamp,
  ),
  _Case(
    'width raised by box minimum',
    _TextRequest(
      itemId: 0x1003,
      text: 'Hi',
      constraints: BoxConstraints(
        minWidth: 80.125,
        maxWidth: 137.75,
        minHeight: 18.5,
        maxHeight: 90.25,
      ),
    ),
    _Coverage.minWidthClamp,
  ),
];

const List<_Case> _explicitFieldCases = <_Case>[
  _Case(
    'explicit defaults and ellipsis',
    _TextRequest(
      itemId: 0x2001,
      text: 'Explicit fields retain the original constrained paragraph semantics.',
      constraints: BoxConstraints(minWidth: 4.0, maxWidth: 148.0, minHeight: 3.0, maxHeight: 120.0),
    ),
    _Coverage.fieldMatrix,
  ),
  _Case(
    'rtl font metrics control',
    _TextRequest(
      itemId: 0x2002,
      text: 'אבג Venus 123 — Café',
      constraints: BoxConstraints(minWidth: 12.0, maxWidth: 176.0, minHeight: 7.0, maxHeight: 96.0),
      ellipsis: '',
      fontSize: 15.0,
      letterSpacing: 1.1,
      wordSpacing: 0.7,
      height: 0.0,
      scalerParameter: 1.25,
      fontWeight: 700,
      maxLines: 0,
      textDirection: _WireTextDirection.rtl,
      textAlign: _WireTextAlign.end,
      fontStyle: _WireFontStyle.italic,
      leadingDistribution: _WireLeadingDistribution.proportional,
      textWidthBasis: _WireTextWidthBasis.parent,
      textScalerKind: _WireTextScalerKind.linear,
      maxLinesMode: _WireMaxLinesMode.unlimited,
      overflowMode: _WireOverflowMode.clip,
      applyHeightToFirstAscent: _WireBool.falseValue,
      heightMode: _WireHeightMode.fontMetrics,
    ),
    _Coverage.fieldMatrix,
  ),
  _Case(
    'rtl multiplier proportional trimmed ascent',
    _TextRequest(
      itemId: 0x2003,
      text: 'אבג Venus 123 — Café',
      constraints: BoxConstraints(minWidth: 12.0, maxWidth: 176.0, minHeight: 7.0, maxHeight: 96.0),
      ellipsis: '',
      fontSize: 15.0,
      letterSpacing: 1.1,
      wordSpacing: 0.7,
      height: 0.1,
      scalerParameter: 1.25,
      fontWeight: 700,
      maxLines: 0,
      textDirection: _WireTextDirection.rtl,
      textAlign: _WireTextAlign.end,
      fontStyle: _WireFontStyle.italic,
      leadingDistribution: _WireLeadingDistribution.proportional,
      textWidthBasis: _WireTextWidthBasis.parent,
      textScalerKind: _WireTextScalerKind.linear,
      maxLinesMode: _WireMaxLinesMode.unlimited,
      overflowMode: _WireOverflowMode.clip,
      applyHeightToFirstAscent: _WireBool.falseValue,
      heightMode: _WireHeightMode.multiplier,
    ),
    _Coverage.fieldMatrix,
  ),
  _Case(
    'rtl multiplier proportional full ascent',
    _TextRequest(
      itemId: 0x2004,
      text: 'אבג Venus 123 — Café',
      constraints: BoxConstraints(minWidth: 12.0, maxWidth: 176.0, minHeight: 7.0, maxHeight: 96.0),
      ellipsis: '',
      fontSize: 15.0,
      letterSpacing: 1.1,
      wordSpacing: 0.7,
      height: 0.1,
      scalerParameter: 1.25,
      fontWeight: 700,
      maxLines: 0,
      textDirection: _WireTextDirection.rtl,
      textAlign: _WireTextAlign.end,
      fontStyle: _WireFontStyle.italic,
      leadingDistribution: _WireLeadingDistribution.proportional,
      textWidthBasis: _WireTextWidthBasis.parent,
      textScalerKind: _WireTextScalerKind.linear,
      maxLinesMode: _WireMaxLinesMode.unlimited,
      overflowMode: _WireOverflowMode.clip,
      heightMode: _WireHeightMode.multiplier,
    ),
    _Coverage.fieldMatrix,
  ),
  _Case(
    'rtl multiplier even full ascent',
    _TextRequest(
      itemId: 0x2005,
      text: 'אבג Venus 123 — Café',
      constraints: BoxConstraints(minWidth: 12.0, maxWidth: 176.0, minHeight: 7.0, maxHeight: 96.0),
      ellipsis: '',
      fontSize: 15.0,
      letterSpacing: 1.1,
      wordSpacing: 0.7,
      height: 0.1,
      scalerParameter: 1.25,
      fontWeight: 700,
      maxLines: 0,
      textDirection: _WireTextDirection.rtl,
      textAlign: _WireTextAlign.end,
      fontStyle: _WireFontStyle.italic,
      leadingDistribution: _WireLeadingDistribution.even,
      textWidthBasis: _WireTextWidthBasis.parent,
      textScalerKind: _WireTextScalerKind.linear,
      maxLinesMode: _WireMaxLinesMode.unlimited,
      overflowMode: _WireOverflowMode.clip,
      heightMode: _WireHeightMode.multiplier,
    ),
    _Coverage.fieldMatrix,
  ),
  _Case(
    'multibyte centered bounded line',
    _TextRequest(
      itemId: 0x2006,
      text: 'Café naïve — Привет Flutter text overflow',
      constraints: BoxConstraints(minWidth: 9.0, maxWidth: 126.0, minHeight: 8.0, maxHeight: 84.0),
      fontSize: 18.0,
      letterSpacing: -0.25,
      wordSpacing: 1.25,
      height: 1.1,
      fontWeight: 300,
      maxLines: 1,
      textAlign: _WireTextAlign.center,
      applyHeightToLastDescent: _WireBool.falseValue,
    ),
    _Coverage.fieldMatrix,
  ),
];

Uint8List _canonicalResultFixture() {
  final Uint8List bytes = Uint8List(_resultHeaderSize + _resultItemStride);
  final ByteData data = ByteData.sublistView(bytes);
  bytes.setRange(0, 4, ascii.encode('VTLR'));
  data.setUint16(4, _schemaVersion, Endian.little);
  data.setUint16(6, _resultHeaderSize, Endian.little);
  data.setUint32(8, bytes.length, Endian.little);
  data.setUint32(12, _BatchStatus.ok, Endian.little);
  data.setUint32(16, 1, Endian.little);
  data.setUint32(20, _resultItemStride, Endian.little);
  data.setUint32(24, _resultHeaderSize, Endian.little);
  data.setUint32(28, _noErrorOffset, Endian.little);
  data.setUint32(32, _BatchErrorDetail.none, Endian.little);
  data.setUint64(40, 0x1234, Endian.little);
  data.setUint32(_resultHeaderSize, _resultItemStride, Endian.little);
  data.setUint64(_resultHeaderSize + 8, 0x4321, Endian.little);
  for (int offset = 16; offset <= 80; offset += 8) {
    data.setFloat64(_resultHeaderSize + offset, offset / 8.0, Endian.little);
  }
  data.setUint32(_resultHeaderSize + 88, 2, Endian.little);
  data.setUint32(_resultHeaderSize + 92, 1, Endian.little);
  return bytes;
}

void _expectRequestFields(Uint8List bytes, List<_TextRequest> items, int batchId) {
  final ByteData data = ByteData.sublistView(bytes);
  expect(ascii.decode(bytes.sublist(0, 4)), 'VTLB');
  expect(data.getUint16(4, Endian.little), _schemaVersion);
  expect(data.getUint16(6, Endian.little), _requestHeaderSize);
  expect(data.getUint32(8, Endian.little), bytes.length);
  expect(data.getUint32(12, Endian.little), 0);
  expect(data.getUint32(16, Endian.little), items.length);
  expect(data.getUint32(20, Endian.little), _requestItemStride);
  expect(data.getUint32(24, Endian.little), _requestHeaderSize);
  final int payloadOffset = _requestHeaderSize + items.length * _requestItemStride;
  expect(data.getUint32(28, Endian.little), payloadOffset);
  expect(data.getUint32(32, Endian.little), bytes.length - payloadOffset);
  expect(data.getUint32(36, Endian.little), 0);
  expect(data.getUint64(40, Endian.little), batchId);

  for (int index = 0; index < items.length; index += 1) {
    final _TextRequest item = items[index];
    final int base = _requestHeaderSize + index * _requestItemStride;
    expect(data.getUint32(base, Endian.little), _requestItemStride);
    expect(data.getUint32(base + 4, Endian.little), 0);
    expect(data.getUint64(base + 8, Endian.little), item.itemId);
    expect(_readPayload(bytes, data, base + 16), item.text);
    expect(_readPayload(bytes, data, base + 24), item.fontFamily);
    expect(_readPayload(bytes, data, base + 32), item.locale);
    if (item.overflowMode == _WireOverflowMode.clip) {
      expect(data.getUint32(base + 40, Endian.little), 0);
      expect(data.getUint32(base + 44, Endian.little), 0);
    } else {
      expect(_readPayload(bytes, data, base + 40), item.ellipsis);
    }
    final List<double> doubles = <double>[
      item.constraints.minWidth,
      item.constraints.maxWidth,
      item.constraints.minHeight,
      item.constraints.maxHeight,
      item.fontSize,
      item.letterSpacing,
      item.wordSpacing,
      item.height,
      item.scalerParameter,
    ];
    for (int doubleIndex = 0; doubleIndex < doubles.length; doubleIndex += 1) {
      expect(
        data.getUint64(base + 48 + doubleIndex * 8, Endian.little),
        _float64Bits(doubles[doubleIndex]),
      );
    }
    expect(data.getUint32(base + 120, Endian.little), item.fontWeight);
    expect(data.getUint32(base + 124, Endian.little), item.maxLines);
    expect(data.getUint16(base + 128, Endian.little), 1);
    expect(data.getUint16(base + 130, Endian.little), item.textDirection.value);
    expect(data.getUint16(base + 132, Endian.little), item.textAlign.value);
    expect(data.getUint16(base + 134, Endian.little), item.fontStyle.value);
    expect(data.getUint16(base + 136, Endian.little), item.leadingDistribution.value);
    expect(data.getUint16(base + 138, Endian.little), item.textWidthBasis.value);
    expect(data.getUint16(base + 140, Endian.little), item.textScalerKind.value);
    expect(data.getUint16(base + 142, Endian.little), item.maxLinesMode.value);
    expect(data.getUint16(base + 144, Endian.little), item.overflowMode.value);
    expect(data.getUint16(base + 146, Endian.little), item.softWrap.value);
    expect(data.getUint16(base + 148, Endian.little), item.applyHeightToFirstAscent.value);
    expect(data.getUint16(base + 150, Endian.little), item.applyHeightToLastDescent.value);
    expect(data.getUint16(base + 152, Endian.little), item.heightMode.value);
    expect(data.getUint16(base + 154, Endian.little), 1);
    expect(data.getUint32(base + 156, Endian.little), 0);
  }
}

String _readPayload(Uint8List bytes, ByteData data, int refOffset) {
  final int offset = data.getUint32(refOffset, Endian.little);
  final int length = data.getUint32(refOffset + 4, Endian.little);
  return utf8.decode(bytes.sublist(offset, offset + length));
}

Future<List<_FlutterMetrics>> _expectGeometryBatch(
  WidgetTester tester,
  List<_Case> cases, {
  required int batchId,
}) async {
  final Uint8List request = _RequestCodec.encode(<_TextRequest>[
    for (final _Case testCase in cases) testCase.request,
  ], batchId: batchId);
  int nativeCallCount = 0;
  Uint8List invokeOnce(Uint8List input) {
    nativeCallCount += 1;
    return ui.debugMeasureTextBatchForVenusPhase1(input);
  }

  final _BatchResult result = _BatchResult.decode(invokeOnce(request));
  expect(nativeCallCount, 1, reason: 'The geometry batch must cross Dart/native exactly once.');
  expect(result.status, _BatchStatus.ok, reason: 'The native text-layout batch must succeed.');
  expect(result.batchId, batchId);
  expect(result.items, hasLength(cases.length));
  final List<_FlutterMetrics> flutterMetrics = <_FlutterMetrics>[];
  for (int index = 0; index < cases.length; index += 1) {
    final _Case testCase = cases[index];
    final _FlutterMetrics flutter = await _measureFlutter(tester, testCase);
    flutterMetrics.add(flutter);
    switch (testCase.coverage) {
      case _Coverage.wrappedLoose:
        expect(flutter.lineCount, greaterThan(1));
        expect(flutter.didExceedMaxLines, isTrue);
        expect(flutter.painterWidth, lessThan(testCase.request.constraints.maxWidth));
      case _Coverage.maxHeightClamp:
        expect(flutter.painterHeight, greaterThan(testCase.request.constraints.maxHeight));
        expect(
          _float64Bits(flutter.boxHeight),
          _float64Bits(testCase.request.constraints.maxHeight),
        );
      case _Coverage.minWidthClamp:
        expect(flutter.maxIntrinsicWidth, lessThan(testCase.request.constraints.minWidth));
        expect(
          _float64Bits(flutter.painterWidth),
          _float64Bits(testCase.request.constraints.minWidth),
        );
        expect(_float64Bits(flutter.boxWidth), _float64Bits(testCase.request.constraints.minWidth));
      case _Coverage.fieldMatrix:
        break;
    }
    _expectResultMatches(result.items[index], flutter, testCase);
  }
  return flutterMetrics;
}

List<int> _behaviorBits(_FlutterMetrics metrics) => <int>[
  _float64Bits(metrics.painterWidth),
  _float64Bits(metrics.painterHeight),
  _float64Bits(metrics.alphabeticBaseline),
  _float64Bits(metrics.ideographicBaseline),
  _float64Bits(metrics.firstLineLeft),
];

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  setUpAll(() async {
    final String flutterRoot = Platform.environment['FLUTTER_ROOT'] ?? Directory.current.path;
    final File font = File(
      '$flutterRoot/engine/src/flutter/txt/third_party/fonts/Roboto-Regular.ttf',
    );
    expect(font.existsSync(), isTrue, reason: 'Roboto fixture missing at ${font.path}');
    await ui.loadFontFromList(await font.readAsBytes(), fontFamily: _fontFamily);
  });

  test('request encoder writes the complete V1 field matrix in little endian', () {
    final List<_TextRequest> items = <_TextRequest>[
      for (final _Case testCase in _explicitFieldCases) testCase.request,
    ];
    final Uint8List request = _RequestCodec.encode(items, batchId: 0x8877665544332211);
    _expectRequestFields(request, items, 0x8877665544332211);

    final ByteData data = ByteData.sublistView(request);
    final int firstBase = _requestHeaderSize;
    final int secondBase = firstBase + _requestItemStride;
    expect(
      data.getUint32(firstBase + 24, Endian.little),
      data.getUint32(secondBase + 24, Endian.little),
      reason: 'The shared Roboto family must reuse one read-only payload range.',
    );
    expect(
      data.getUint32(firstBase + 32, Endian.little),
      data.getUint32(secondBase + 32, Endian.little),
      reason: 'The shared locale must reuse one read-only payload range.',
    );
  });

  test('result decoder accepts only canonical V1 envelopes and item records', () {
    final Uint8List fixture = _canonicalResultFixture();
    final _BatchResult result = _BatchResult.decode(fixture);
    expect(result.status, _BatchStatus.ok);
    expect(result.batchId, 0x1234);
    expect(result.items, hasLength(1));
    expect(result.items.single.itemId, 0x4321);
    expect(result.items.single.painterWidthBits, _float64Bits(2.0));
    expect(result.items.single.firstLineLeftBits, _float64Bits(10.0));
    expect(result.items.single.lineCount, 2);
    expect(result.items.single.didExceedMaxLines, isTrue);

    final Uint8List badMagic = Uint8List.fromList(fixture)..[0] = 0;
    expect(() => _BatchResult.decode(badMagic), throwsFormatException);
    final Uint8List badSize = Uint8List.fromList(fixture);
    ByteData.sublistView(badSize).setUint32(8, badSize.length - 1, Endian.little);
    expect(() => _BatchResult.decode(badSize), throwsFormatException);
    final Uint8List badBool = Uint8List.fromList(fixture);
    ByteData.sublistView(badBool).setUint32(_resultHeaderSize + 92, 2, Endian.little);
    expect(() => _BatchResult.decode(badBool), throwsFormatException);
    final Uint8List badFlags = Uint8List.fromList(fixture);
    ByteData.sublistView(badFlags).setUint32(_resultHeaderSize + 96, 1, Endian.little);
    expect(() => _BatchResult.decode(badFlags), throwsFormatException);
  });

  test('batch oracle rejects a worker isolate before acquiring input', () async {
    await expectLater(
      Isolate.run<void>(() {
        ui.debugMeasureTextBatchForVenusPhase1(Uint8List(0));
      }),
      throwsA(equals('UI actions are only available on root isolate.')),
    );
  });

  testWidgets('one native batch exactly matches all three constrained RenderParagraph cases', (
    WidgetTester tester,
  ) async {
    await _expectGeometryBatch(tester, _coreCases, batchId: _coreBatchId);
  });

  testWidgets('explicit text fields map to the same Flutter paragraph semantics', (
    WidgetTester tester,
  ) async {
    final List<_FlutterMetrics> metrics = await _expectGeometryBatch(
      tester,
      _explicitFieldCases,
      batchId: 0x0203040506070809,
    );
    expect(
      _behaviorBits(metrics[1]),
      isNot(equals(_behaviorBits(metrics[2]))),
      reason: 'font-metrics and multiplier height modes must not collapse to one mapping.',
    );
    expect(
      _behaviorBits(metrics[2]),
      isNot(equals(_behaviorBits(metrics[3]))),
      reason: 'applyHeightToFirstAscent=false must change multiplier-height geometry.',
    );
  });

  test('malformed batch headers return exact canonical global failures', () {
    final Uint8List valid = _RequestCodec.encode(<_TextRequest>[
      _coreCases.first.request,
    ], batchId: _coreBatchId);
    final List<
      ({String name, Uint8List Function() input, int status, int detail, int offset, int batchId})
    >
    failures =
        <
          ({
            String name,
            Uint8List Function() input,
            int status,
            int detail,
            int offset,
            int batchId,
          })
        >[
          (
            name: 'truncated magic',
            input: () => Uint8List.sublistView(valid, 0, 20),
            status: _BatchStatus.invalidHeader,
            detail: 1,
            offset: 20,
            batchId: 0,
          ),
          (
            name: 'bad magic',
            input: () => Uint8List.fromList(valid)..[0] = 0,
            status: _BatchStatus.invalidHeader,
            detail: _BatchErrorDetail.badMagic,
            offset: 0,
            batchId: 0,
          ),
          (
            name: 'bad version',
            input: () {
              final Uint8List bytes = Uint8List.fromList(valid);
              ByteData.sublistView(bytes).setUint16(4, 2, Endian.little);
              return bytes;
            },
            status: _BatchStatus.unsupportedVersion,
            detail: _BatchErrorDetail.badVersion,
            offset: 4,
            batchId: 0,
          ),
          (
            name: 'bad header size',
            input: () {
              final Uint8List bytes = Uint8List.fromList(valid);
              ByteData.sublistView(bytes).setUint16(6, 40, Endian.little);
              return bytes;
            },
            status: _BatchStatus.invalidHeader,
            detail: _BatchErrorDetail.badHeaderSize,
            offset: 6,
            batchId: 0,
          ),
          (
            name: 'bad byte size',
            input: () {
              final Uint8List bytes = Uint8List.fromList(valid);
              ByteData.sublistView(bytes).setUint32(8, bytes.length - 1, Endian.little);
              return bytes;
            },
            status: _BatchStatus.invalidHeader,
            detail: _BatchErrorDetail.badByteSize,
            offset: 8,
            batchId: _coreBatchId,
          ),
          (
            name: 'nonzero flags',
            input: () {
              final Uint8List bytes = Uint8List.fromList(valid);
              ByteData.sublistView(bytes).setUint32(12, 1, Endian.little);
              return bytes;
            },
            status: _BatchStatus.invalidHeader,
            detail: _BatchErrorDetail.nonzeroFlags,
            offset: 12,
            batchId: _coreBatchId,
          ),
          (
            name: 'bad item stride',
            input: () {
              final Uint8List bytes = Uint8List.fromList(valid);
              ByteData.sublistView(bytes).setUint32(20, 152, Endian.little);
              return bytes;
            },
            status: _BatchStatus.invalidTable,
            detail: _BatchErrorDetail.badItemStride,
            offset: 20,
            batchId: _coreBatchId,
          ),
          (
            name: 'unaligned items offset',
            input: () {
              final Uint8List bytes = Uint8List.fromList(valid);
              ByteData.sublistView(bytes).setUint32(24, 49, Endian.little);
              return bytes;
            },
            status: _BatchStatus.invalidTable,
            detail: _BatchErrorDetail.itemsOffsetAlignment,
            offset: 24,
            batchId: _coreBatchId,
          ),
          (
            name: 'unaligned payload offset',
            input: () {
              final Uint8List bytes = Uint8List.fromList(valid);
              ByteData.sublistView(bytes).setUint32(28, 49, Endian.little);
              return bytes;
            },
            status: _BatchStatus.invalidTable,
            detail: _BatchErrorDetail.payloadOffsetAlignment,
            offset: 28,
            batchId: _coreBatchId,
          ),
          (
            name: 'nonzero reserved',
            input: () {
              final Uint8List bytes = Uint8List.fromList(valid);
              ByteData.sublistView(bytes).setUint32(36, 1, Endian.little);
              return bytes;
            },
            status: _BatchStatus.invalidHeader,
            detail: _BatchErrorDetail.nonzeroReserved,
            offset: 36,
            batchId: _coreBatchId,
          ),
        ];

    for (final failure in failures) {
      final _BatchResult result = _BatchResult.decode(
        ui.debugMeasureTextBatchForVenusPhase1(failure.input()),
      );
      expect(result.status, failure.status, reason: failure.name);
      expect(result.errorDetail, failure.detail, reason: failure.name);
      expect(result.firstErrorOffset, failure.offset, reason: failure.name);
      expect(result.batchId, failure.batchId, reason: failure.name);
      expect(result.items, isEmpty, reason: failure.name);
    }
  });

  test('invalid items are isolated and duplicate ids are first-wins', () {
    final List<_TextRequest> items = <_TextRequest>[
      _coreCases[0].request,
      _coreCases[1].request,
      _coreCases[2].request,
      const _TextRequest(
        itemId: 0x1004,
        text: 'duplicate id payload',
        constraints: BoxConstraints(
          minWidth: 2.0,
          maxWidth: 140.0,
          minHeight: 1.0,
          maxHeight: 100.0,
        ),
      ),
    ];
    final Uint8List request = _RequestCodec.encode(items, batchId: 0x3030);
    final ByteData data = ByteData.sublistView(request);
    data.setUint16(_requestHeaderSize + _requestItemStride + 130, 99, Endian.little);
    data.setUint64(
      _requestHeaderSize + 3 * _requestItemStride + 8,
      items.first.itemId,
      Endian.little,
    );

    final _BatchResult result = _BatchResult.decode(
      ui.debugMeasureTextBatchForVenusPhase1(request),
    );
    expect(result.status, _BatchStatus.ok);
    expect(result.items, hasLength(4));
    expect(result.items[0].status, _ItemStatus.ok);
    expect(result.items[1].status, _ItemStatus.invalidEnum);
    expect(result.items[1].errorFieldId, _ErrorFieldId.textDirection);
    expect(result.items[1].errorDetail, _ItemErrorDetail.aboveMax);
    expect(result.items[2].status, _ItemStatus.ok);
    expect(result.items[3].status, _ItemStatus.duplicateItemId);
    expect(result.items[3].itemId, items.first.itemId);
    expect(result.items[3].errorFieldId, _ErrorFieldId.duplicateId);
    expect(result.items[3].errorDetail, _ItemErrorDetail.duplicateLaterOccurrence);
  });
}
