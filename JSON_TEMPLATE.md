# 课表 JSON 模板说明

本文件用于记录 `schedule.json` 的数据格式，供 ESP32 日历程序读取。

## 1. 文件要求

- 文件名：`schedule.json`
- 编码：UTF-8
- 时间格式：`HH:mm`
- 星期编号：

| 数值 | 星期 |
|---:|---|
| 1 | 星期一 |
| 2 | 星期二 |
| 3 | 星期三 |
| 4 | 星期四 |
| 5 | 星期五 |
| 6 | 星期六 |
| 7 | 星期日 |

## 2. 顶层结构

```json
{
  "schemaVersion": 1,
  "semester": {
    "name": "2025-2026学年 夏季学期",
    "startDate": null,
    "endDate": null
  },
  "weekdayConvention": "1=星期一, 2=星期二, 3=星期三, 4=星期四, 5=星期五, 6=星期六, 7=星期日",
  "periods": [],
  "courses": []
}
```

字段说明：

| 字段 | 类型 | 说明 |
|---|---|---|
| `schemaVersion` | 整数 | JSON 格式版本。以后修改结构时递增 |
| `semester.name` | 字符串 | 学期名称 |
| `semester.startDate` | 字符串或 `null` | 学期开始日期，格式为 `YYYY-MM-DD` |
| `semester.endDate` | 字符串或 `null` | 学期结束日期，格式为 `YYYY-MM-DD` |
| `periods` | 数组 | 节次与起止时间表 |
| `courses` | 数组 | 课程列表 |

> 当前课表截图没有提供夏季学期的具体起止日期，因此 `startDate` 和 `endDate` 暂时设为 `null`。后续确认日期后再填写。

## 3. 节次模板

```json
{
  "period": 1,
  "start": "08:00",
  "end": "08:45"
}
```

字段说明：

| 字段 | 类型 | 说明 |
|---|---|---|
| `period` | 整数 | 第几节课 |
| `start` | 字符串 | 开始时间 |
| `end` | 字符串 | 结束时间 |

课程时间由 `startPeriod` 和 `endPeriod` 对应到此表。

例如：

```json
{
  "startPeriod": 4,
  "endPeriod": 5
}
```

表示课程从第 4 节开始，到第 5 节结束，即 `11:05~15:15`。其中会跨越教务课表中的中午休息时间。

## 4. 课程模板

```json
{
  "id": "course_001",
  "name": "课程名称",
  "teachers": [
    "教师甲",
    "教师乙"
  ],
  "sessions": []
}
```

字段说明：

| 字段 | 类型 | 说明 |
|---|---|---|
| `id` | 字符串 | 课程唯一标识，不应与其他课程重复 |
| `name` | 字符串 | 课程名称 |
| `teachers` | 字符串数组 | 课程默认教师列表 |
| `sessions` | 数组 | 该课程的具体上课安排 |

## 5. 上课安排模板

```json
{
  "weekday": 1,
  "weeks": [
    1,
    2,
    3
  ],
  "startPeriod": 4,
  "endPeriod": 5,
  "location": "教学楼101"
}
```

字段说明：

| 字段 | 类型 | 说明 |
|---|---|---|
| `weekday` | 整数 | 星期编号，星期一为 1 |
| `weeks` | 整数数组 | 上课周次 |
| `startPeriod` | 整数 | 起始节次 |
| `endPeriod` | 整数 | 结束节次 |
| `location` | 字符串 | 上课地点 |
| `teachers` | 字符串数组，可选 | 仅当本次课教师与课程默认教师不同时填写 |

例如：

```json
{
  "weekday": 4,
  "weeks": [
    1,
    2,
    3
  ],
  "startPeriod": 5,
  "endPeriod": 7,
  "location": "南存相楼C304",
  "teachers": [
    "陈建发",
    "孙振宇",
    "施俊杰"
  ]
}
```

该条表示：

- 第 1、2、3 周上课；
- 星期四；
- 第 5 至第 7 节；
- 地点为南存相楼 C304；
- 本次课教师覆盖课程默认教师列表。

## 6. 完整空白模板

```json
{
  "schemaVersion": 1,
  "semester": {
    "name": "学期名称",
    "startDate": "YYYY-MM-DD",
    "endDate": "YYYY-MM-DD"
  },
  "weekdayConvention": "1=星期一, 2=星期二, 3=星期三, 4=星期四, 5=星期五, 6=星期六, 7=星期日",
  "periods": [
    {
      "period": 1,
      "start": "08:00",
      "end": "08:45"
    }
  ],
  "courses": [
    {
      "id": "course_001",
      "name": "课程名称",
      "teachers": [
        "教师姓名"
      ],
      "sessions": [
        {
          "weekday": 1,
          "weeks": [
            1
          ],
          "startPeriod": 1,
          "endPeriod": 2,
          "location": "教学地点"
        }
      ]
    }
  ]
}
```

## 7. ESP32 解析建议

程序读取课表时，可以按以下顺序处理：

1. 根据当前日期和 `semester.startDate` 计算当前教学周；
2. 将当前星期转换为 `1~7`；
3. 遍历 `courses`；
4. 遍历课程中的 `sessions`；
5. 判断当前周是否存在于 `weeks` 中；
6. 判断 `weekday` 是否等于当前星期；
7. 根据 `startPeriod` 和 `endPeriod` 查询 `periods`，得到课程起止时间。

若暂时不填写 `semester.startDate`，可以先由单片机配置页或程序常量手动指定当前周数。
