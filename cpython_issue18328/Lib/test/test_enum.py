import enum
import unittest
from collections import OrderedDict
from pickle import dumps, loads, PicklingError
from enum import Enum, IntEnum

# for pickle tests
try:
    class Stooges(Enum):
        LARRY = 1
        CURLY = 2
        MOE = 3
except Exception as exc:
    Stooges = exc

try:
    class IntStooges(int, Enum):
        LARRY = 1
        CURLY = 2
        MOE = 3
except Exception as exc:
    IntStooges = exc

try:
    class FloatStooges(float, Enum):
        LARRY = 1.39
        CURLY = 2.72
        MOE = 3.142596
except Exception as exc:
    FloatStooges = exc

# for pickle test and subclass tests
try:
    class StrEnum(str, Enum):
        'accepts only string values'
    class Name(StrEnum):
        BDFL = 'Guido van Rossum'
        FLUFL = 'Barry Warsaw'
except Exception as exc:
    Name = exc

try:
    Question = Enum('Question', 'who what when where why', module=__name__)
except Exception as exc:
    Question = exc

try:
    Answer = Enum('Answer', 'him this then there because')
except Exception as exc:
    Answer = exc

# for doctests
try:
    class Fruit(Enum):
        tomato = 1
        banana = 2
        cherry = 3
except Exception:
    pass

class TestEnum(unittest.TestCase):
    def setUp(self):
        class Season(Enum):
            SPRING = 1
            SUMMER = 2
            AUTUMN = 3
            WINTER = 4
        self.Season = Season

    def test_enum_in_enum_out(self):
        Season = self.Season
        self.assertIs(Season(Season.WINTER), Season.WINTER)

    def test_enum_value(self):
        Season = self.Season
        self.assertEqual(Season.SPRING.value, 1)

    def test_intenum_value(self):
        self.assertEqual(IntStooges.CURLY.value, 2)

    def test_dir_on_class(self):
        Season = self.Season
        self.assertEqual(
            set(dir(Season)),
            set(['__class__', '__doc__', '__members__',
                'SPRING', 'SUMMER', 'AUTUMN', 'WINTER']),
            )

    def test_dir_on_item(self):
        Season = self.Season
        self.assertEqual(
            set(dir(Season.WINTER)),
            set(['__class__', '__doc__', 'name', 'value']),
            )

    def test_enum(self):
        Season = self.Season
        lst = list(Season)
        self.assertEqual(len(lst), len(Season))
        self.assertEqual(len(Season), 4, Season)
        self.assertEqual(
            [Season.SPRING, Season.SUMMER, Season.AUTUMN, Season.WINTER], lst)

        for i, season in enumerate('SPRING SUMMER AUTUMN WINTER'.split(), 1):
            e = Season(i)
            self.assertEqual(e, getattr(Season, season))
            self.assertEqual(e.value, i)
            self.assertNotEqual(e, i)
            self.assertEqual(e.name, season)
            self.assertIn(e, Season)
            self.assertIs(type(e), Season)
            self.assertIsInstance(e, Season)
            self.assertEqual(str(e), 'Season.' + season)
            self.assertEqual(
                    repr(e),
                    '<Season.{0}: {1}>'.format(season, i),
                    )

    def test_value_name(self):
        Season = self.Season
        self.assertEqual(Season.SPRING.name, 'SPRING')
        self.assertEqual(Season.SPRING.value, 1)
        with self.assertRaises(AttributeError):
            Season.SPRING.name = 'invierno'
        with self.assertRaises(AttributeError):
            Season.SPRING.value = 2

    def test_invalid_names(self):
        with self.assertRaises(ValueError):
            class Wrong(Enum):
                mro = 9
        with self.assertRaises(ValueError):
            class Wrong(Enum):
                _create_= 11
        with self.assertRaises(ValueError):
            class Wrong(Enum):
                _get_mixins_ = 9
        with self.assertRaises(ValueError):
            class Wrong(Enum):
                _find_new_ = 1
        with self.assertRaises(ValueError):
            class Wrong(Enum):
                _any_name_ = 9

    def test_contains(self):
        Season = self.Season
        self.assertIn(Season.AUTUMN, Season)
        self.assertNotIn(3, Season)

        val = Season(3)
        self.assertIn(val, Season)

        class OtherEnum(Enum):
            one = 1; two = 2
        self.assertNotIn(OtherEnum.two, Season)

    def test_comparisons(self):
        Season = self.Season
        with self.assertRaises(TypeError):
            Season.SPRING < Season.WINTER
        with self.assertRaises(TypeError):
            Season.SPRING > 4

        self.assertNotEqual(Season.SPRING, 1)

        class Part(Enum):
            SPRING = 1
            CLIP = 2
            BARREL = 3

        self.assertNotEqual(Season.SPRING, Part.SPRING)
        with self.assertRaises(TypeError):
            Season.SPRING < Part.CLIP

    def test_enum_duplicates(self):
        class Season(Enum):
            SPRING = 1
            SUMMER = 2
            AUTUMN = FALL = 3
            WINTER = 4
            ANOTHER_SPRING = 1
        lst = list(Season)
        self.assertEqual(
            lst,
            [Season.SPRING, Season.SUMMER,
             Season.AUTUMN, Season.WINTER,
            ])
        self.assertIs(Season.FALL, Season.AUTUMN)
        self.assertEqual(Season.FALL.value, 3)
        self.assertEqual(Season.AUTUMN.value, 3)
        self.assertIs(Season(3), Season.AUTUMN)
        self.assertIs(Season(1), Season.SPRING)
        self.assertEqual(Season.FALL.name, 'AUTUMN')
        self.assertEqual(
                [k for k,v in Season.__members__.items() if v.name != k],
                ['FALL', 'ANOTHER_SPRING'],
                )

    def test_enum_with_value_name(self):
        class Huh(Enum):
            name = 1
            value = 2
        self.assertEqual(
            list(Huh),
            [Huh.name, Huh.value],
            )
        self.assertIs(type(Huh.name), Huh)
        self.assertEqual(Huh.name.name, 'name')
        self.assertEqual(Huh.name.value, 1)
    def test_hash(self):
        Season = self.Season
        dates = {}
        dates[Season.WINTER] = '1225'
        dates[Season.SPRING] = '0315'
        dates[Season.SUMMER] = '0704'
        dates[Season.AUTUMN] = '1031'
        self.assertEqual(dates[Season.AUTUMN], '1031')

    def test_intenum_from_scratch(self):
        class phy(int, Enum):
            pi = 3
            tau = 2 * pi
        self.assertTrue(phy.pi < phy.tau)

    def test_intenum_inherited(self):
        class IntEnum(int, Enum):
            pass
        class phy(IntEnum):
            pi = 3
            tau = 2 * pi
        self.assertTrue(phy.pi < phy.tau)

    def test_floatenum_from_scratch(self):
        class phy(float, Enum):
            pi = 3.141596
            tau = 2 * pi
        self.assertTrue(phy.pi < phy.tau)

    def test_floatenum_inherited(self):
        class FloatEnum(float, Enum):
            pass
        class phy(FloatEnum):
            pi = 3.141596
            tau = 2 * pi
        self.assertTrue(phy.pi < phy.tau)

    def test_strenum_from_scratch(self):
        class phy(str, Enum):
            pi = 'Pi'
            tau = 'Tau'
        self.assertTrue(phy.pi < phy.tau)

    def test_strenum_inherited(self):
        class StrEnum(str, Enum):
            pass
        class phy(StrEnum):
            pi = 'Pi'
            tau = 'Tau'
        self.assertTrue(phy.pi < phy.tau)


    def test_intenum(self):
        class WeekDay(IntEnum):
            SUNDAY = 1
            MONDAY = 2
            TUESDAY = 3
            WEDNESDAY = 4
            THURSDAY = 5
            FRIDAY = 6
            SATURDAY = 7

        self.assertEqual(['a', 'b', 'c'][WeekDay.MONDAY], 'c')
        self.assertEqual([i for i in range(WeekDay.TUESDAY)], [0, 1, 2])

        lst = list(WeekDay)
        self.assertEqual(len(lst), len(WeekDay))
        self.assertEqual(len(WeekDay), 7)
        target = 'SUNDAY MONDAY TUESDAY WEDNESDAY THURSDAY FRIDAY SATURDAY'
        target = target.split()
        for i, weekday in enumerate(target, 1):
            e = WeekDay(i)
            self.assertEqual(e, i)
            self.assertEqual(int(e), i)
            self.assertEqual(e.name, weekday)
            self.assertIn(e, WeekDay)
            self.assertEqual(lst.index(e)+1, i)
            self.assertTrue(0 < e < 8)
            self.assertIs(type(e), WeekDay)
            self.assertIsInstance(e, int)
            self.assertIsInstance(e, Enum)

    def test_intenum_duplicates(self):
        class WeekDay(IntEnum):
            SUNDAY = 1
            MONDAY = 2
            TUESDAY = TEUSDAY = 3
            WEDNESDAY = 4
            THURSDAY = 5
            FRIDAY = 6
            SATURDAY = 7
        self.assertIs(WeekDay.TEUSDAY, WeekDay.TUESDAY)
        self.assertEqual(WeekDay(3).name, 'TUESDAY')
        self.assertEqual([k for k,v in WeekDay.__members__.items()
                if v.name != k], ['TEUSDAY', ])

    def test_pickle_enum(self):
        if isinstance(Stooges, Exception):
            raise Stooges
        self.assertIs(Stooges.CURLY, loads(dumps(Stooges.CURLY)))
        self.assertIs(Stooges, loads(dumps(Stooges)))

    def test_pickle_int(self):
        if isinstance(IntStooges, Exception):
            raise IntStooges
        self.assertIs(IntStooges.CURLY, loads(dumps(IntStooges.CURLY)))
        self.assertIs(IntStooges, loads(dumps(IntStooges)))

    def test_pickle_float(self):
        if isinstance(FloatStooges, Exception):
            raise FloatStooges
        self.assertIs(FloatStooges.CURLY, loads(dumps(FloatStooges.CURLY)))
        self.assertIs(FloatStooges, loads(dumps(FloatStooges)))

    def test_pickle_enum_function(self):
        if isinstance(Answer, Exception):
            raise Answer
        self.assertIs(Answer.him, loads(dumps(Answer.him)))
        self.assertIs(Answer, loads(dumps(Answer)))

    def test_pickle_enum_function_with_module(self):
        if isinstance(Question, Exception):
            raise Question
        self.assertIs(Question.who, loads(dumps(Question.who)))
        self.assertIs(Question, loads(dumps(Question)))

    def test_exploding_pickle(self):
        BadPickle = Enum('BadPickle', 'dill sweet bread-n-butter')
        enum._make_class_unpicklable(BadPickle)
        globals()['BadPickle'] = BadPickle
        with self.assertRaises(TypeError):
            dumps(BadPickle.dill)
        with self.assertRaises(PicklingError):
            dumps(BadPickle)

    def test_string_enum(self):
        class SkillLevel(str, Enum):
            master = 'what is the sound of one hand clapping?'
            journeyman = 'why did the chicken cross the road?'
            apprentice = 'knock, knock!'
        self.assertEqual(SkillLevel.apprentice, 'knock, knock!')

    def test_getattr_getitem(self):
        class Period(Enum):
            morning = 1
            noon = 2
            evening = 3
            night = 4
        self.assertIs(Period(2), Period.noon)
        self.assertIs(getattr(Period, 'night'), Period.night)
        self.assertIs(Period['morning'], Period.morning)

    def test_getattr_dunder(self):
        Season = self.Season
        self.assertTrue(getattr(Season, '__eq__'))

    def test_iteration_order(self):
        class Season(Enum):
            SUMMER = 2
            WINTER = 4
            AUTUMN = 3
            SPRING = 1
        self.assertEqual(
                list(Season),
                [Season.SUMMER, Season.WINTER, Season.AUTUMN, Season.SPRING],
                )

    def test_programatic_function_string(self):
        SummerMonth = Enum('SummerMonth', 'june july august')
        lst = list(SummerMonth)
        self.assertEqual(len(lst), len(SummerMonth))
        self.assertEqual(len(SummerMonth), 3, SummerMonth)
        self.assertEqual(
                [SummerMonth.june, SummerMonth.july, SummerMonth.august],
                lst,
                )
        for i, month in enumerate('june july august'.split(), 1):
            e = SummerMonth(i)
            self.assertEqual(int(e.value), i)
            self.assertNotEqual(e, i)
            self.assertEqual(e.name, month)
            self.assertIn(e, SummerMonth)
            self.assertIs(type(e), SummerMonth)

    def test_programatic_function_string_list(self):
        SummerMonth = Enum('SummerMonth', ['june', 'july', 'august'])
        lst = list(SummerMonth)
        self.assertEqual(len(lst), len(SummerMonth))
        self.assertEqual(len(SummerMonth), 3, SummerMonth)
        self.assertEqual(
                [SummerMonth.june, SummerMonth.july, SummerMonth.august],
                lst,
                )
        for i, month in enumerate('june july august'.split(), 1):
            e = SummerMonth(i)
            self.assertEqual(int(e.value), i)
            self.assertNotEqual(e, i)
            self.assertEqual(e.name, month)
            self.assertIn(e, SummerMonth)
            self.assertIs(type(e), SummerMonth)

    def test_programatic_function_iterable(self):
        SummerMonth = Enum(
                'SummerMonth',
                (('june', 1), ('july', 2), ('august', 3))
                )
        lst = list(SummerMonth)
        self.assertEqual(len(lst), len(SummerMonth))
        self.assertEqual(len(SummerMonth), 3, SummerMonth)
        self.assertEqual(
                [SummerMonth.june, SummerMonth.july, SummerMonth.august],
                lst,
                )
        for i, month in enumerate('june july august'.split(), 1):
            e = SummerMonth(i)
            self.assertEqual(int(e.value), i)
            self.assertNotEqual(e, i)
            self.assertEqual(e.name, month)
            self.assertIn(e, SummerMonth)
            self.assertIs(type(e), SummerMonth)

    def test_programatic_function_from_dict(self):
        SummerMonth = Enum(
                'SummerMonth',
                OrderedDict((('june', 1), ('july', 2), ('august', 3)))
                )
        lst = list(SummerMonth)
        self.assertEqual(len(lst), len(SummerMonth))
        self.assertEqual(len(SummerMonth), 3, SummerMonth)
        self.assertEqual(
                [SummerMonth.june, SummerMonth.july, SummerMonth.august],
                lst,
                )
        for i, month in enumerate('june july august'.split(), 1):
            e = SummerMonth(i)
            self.assertEqual(int(e.value), i)
            self.assertNotEqual(e, i)
            self.assertEqual(e.name, month)
            self.assertIn(e, SummerMonth)
            self.assertIs(type(e), SummerMonth)

    def test_programatic_function_type(self):
        SummerMonth = Enum('SummerMonth', 'june july august', type=int)
        lst = list(SummerMonth)
        self.assertEqual(len(lst), len(SummerMonth))
        self.assertEqual(len(SummerMonth), 3, SummerMonth)
        self.assertEqual(
                [SummerMonth.june, SummerMonth.july, SummerMonth.august],
                lst,
                )
        for i, month in enumerate('june july august'.split(), 1):
            e = SummerMonth(i)
            self.assertEqual(e, i)
            self.assertEqual(e.name, month)
            self.assertIn(e, SummerMonth)
            self.assertIs(type(e), SummerMonth)

    def test_programatic_function_type_from_subclass(self):
        SummerMonth = IntEnum('SummerMonth', 'june july august')
        lst = list(SummerMonth)
        self.assertEqual(len(lst), len(SummerMonth))
        self.assertEqual(len(SummerMonth), 3, SummerMonth)
        self.assertEqual(
                [SummerMonth.june, SummerMonth.july, SummerMonth.august],
                lst,
                )
        for i, month in enumerate('june july august'.split(), 1):
            e = SummerMonth(i)
            self.assertEqual(e, i)
            self.assertEqual(e.name, month)
            self.assertIn(e, SummerMonth)
            self.assertIs(type(e), SummerMonth)

    def test_subclassing(self):
        if isinstance(Name, Exception):
            raise Name
        self.assertEqual(Name.BDFL, 'Guido van Rossum')
        self.assertTrue(Name.BDFL, Name('Guido van Rossum'))
        self.assertIs(Name.BDFL, getattr(Name, 'BDFL'))
        self.assertIs(Name.BDFL, loads(dumps(Name.BDFL)))

    def test_extending(self):
        class Color(Enum):
            red = 1
            green = 2
            blue = 3
        with self.assertRaises(TypeError):
            class MoreColor(Color):
                cyan = 4
                magenta = 5
                yellow = 6

    def test_exclude_methods(self):
        class whatever(Enum):
            this = 'that'
            these = 'those'
            def really(self):
                return 'no, not %s' % self.value
        self.assertIsNot(type(whatever.really), whatever)
        self.assertEqual(whatever.this.really(), 'no, not that')

    def test_overwrite_enums(self):
        class Why(Enum):
            question = 1
            answer = 2
            propisition = 3
            def question(self):
                print(42)
        self.assertIsNot(type(Why.question), Why)
        self.assertNotIn(Why.question, Why._member_names)
        self.assertNotIn(Why.question, Why)

    def test_wrong_inheritance_order(self):
        with self.assertRaises(TypeError):
            class Wrong(Enum, str):
                NotHere = 'error before this point'

    def test_intenum_transitivity(self):
        class number(IntEnum):
            one = 1
            two = 2
            three = 3
        class numero(IntEnum):
            uno = 1
            dos = 2
            tres = 3
        self.assertEqual(number.one, numero.uno)
        self.assertEqual(number.two, numero.dos)
        self.assertEqual(number.three, numero.tres)

    def test_wrong_enum_in_call(self):
        class Monochrome(Enum):
            black = 0
            white = 1
        class Gender(Enum):
            male = 0
            female = 1
        self.assertRaises(ValueError, Monochrome, Gender.male)

    def test_wrong_enum_in_mixed_call(self):
        class Monochrome(IntEnum):
            black = 0
            white = 1
        class Gender(Enum):
            male = 0
            female = 1
        self.assertRaises(ValueError, Monochrome, Gender.male)

    def test_mixed_enum_in_call_1(self):
        class Monochrome(IntEnum):
            black = 0
            white = 1
        class Gender(IntEnum):
            male = 0
            female = 1
        self.assertIs(Monochrome(Gender.female), Monochrome.white)

    def test_mixed_enum_in_call_2(self):
        class Monochrome(Enum):
            black = 0
            white = 1
        class Gender(IntEnum):
            male = 0
            female = 1
        self.assertIs(Monochrome(Gender.male), Monochrome.black)

    def test_flufl_enum(self):
        class Fluflnum(Enum):
            def __int__(self):
                return int(self.value)
        class MailManOptions(Fluflnum):
            option1 = 1
            option2 = 2
            option3 = 3
        self.assertEqual(int(MailManOptions.option1), 1)

    def test_no_such_enum_member(self):
        class Color(Enum):
            red = 1
            green = 2
            blue = 3
        with self.assertRaises(ValueError):
            Color(4)
        with self.assertRaises(KeyError):
            Color['chartreuse']

    def test_new_repr(self):
        class Color(Enum):
            red = 1
            green = 2
            blue = 3
            def __repr__(self):
                return "don't you just love shades of %s?" % self.name
        self.assertEqual(
                repr(Color.blue),
                "don't you just love shades of blue?",
                )

    def test_inherited_repr(self):
        class MyEnum(Enum):
            def __repr__(self):
                return "My name is %s." % self.name
        class MyIntEnum(int, MyEnum):
            this = 1
            that = 2
            theother = 3
        self.assertEqual(repr(MyIntEnum.that), "My name is that.")

    def test_multiple_mixin_mro(self):
        class auto_enum(type(Enum)):
            def __new__(metacls, cls, bases, classdict):
                temp = type(classdict)()
                names = set(classdict._member_names)
                i = 0
                for k in classdict._member_names:
                    v = classdict[k]
                    if v is Ellipsis:
                        v = i
                    else:
                        i = v
                    i += 1
                    temp[k] = v
                for k, v in classdict.items():
                    if k not in names:
                        temp[k] = v
                return super(auto_enum, metacls).__new__(
                        metacls, cls, bases, temp)

        class AutoNumberedEnum(Enum, metaclass=auto_enum):
            pass

        class AutoIntEnum(IntEnum, metaclass=auto_enum):
            pass

        class TestAutoNumber(AutoNumberedEnum):
            a = ...
            b = 3
            c = ...

        class TestAutoInt(AutoIntEnum):
            a = ...
            b = 3
            c = ...

    def test_subclasses_with_getnewargs(self):
        class NamedInt(int):
            def __new__(cls, *args):
                _args = args
                name, *args = args
                if len(args) == 0:
                    raise TypeError("name and value must be specified")
                self = int.__new__(cls, *args)
                self._intname = name
                self._args = _args
                return self
            def __getnewargs__(self):
                return self._args
            @property
            def __name__(self):
                return self._intname
            def __repr__(self):
                # repr() is updated to include the name and type info
                return "{}({!r}, {})".format(type(self).__name__,
                                             self.__name__,
                                             int.__repr__(self))
            def __str__(self):
                # str() is unchanged, even if it relies on the repr() fallback
                base = int
                base_str = base.__str__
                if base_str.__objclass__ is object:
                    return base.__repr__(self)
                return base_str(self)
            # for simplicity, we only define one operator that
            # propagates expressions
            def __add__(self, other):
                temp = int(self) + int( other)
                if isinstance(self, NamedInt) and isinstance(other, NamedInt):
                    return NamedInt(
                        '({0} + {1})'.format(self.__name__, other.__name__),
                        temp )
                else:
                    return temp

        class NEI(NamedInt, Enum):
            x = ('the-x', 1)
            y = ('the-y', 2)

        self.assertIs(NEI.__new__, Enum.__new__)
        self.assertEqual(repr(NEI.x + NEI.y), "NamedInt('(the-x + the-y)', 3)")
        globals()['NamedInt'] = NamedInt
        globals()['NEI'] = NEI
        NI5 = NamedInt('test', 5)
        self.assertEqual(NI5, 5)
        self.assertEqual(loads(dumps(NI5)), 5)
        self.assertEqual(NEI.y.value, 2)
        self.assertIs(loads(dumps(NEI.y)), NEI.y)

    def test_subclasses_without_getnewargs(self):
        class NamedInt(int):
            def __new__(cls, *args):
                _args = args
                name, *args = args
                if len(args) == 0:
                    raise TypeError("name and value must be specified")
                self = int.__new__(cls, *args)
                self._intname = name
                self._args = _args
                return self
            @property
            def __name__(self):
                return self._intname
            def __repr__(self):
                # repr() is updated to include the name and type info
                return "{}({!r}, {})".format(type(self).__name__,
                                             self.__name__,
                                             int.__repr__(self))
            def __str__(self):
                # str() is unchanged, even if it relies on the repr() fallback
                base = int
                base_str = base.__str__
                if base_str.__objclass__ is object:
                    return base.__repr__(self)
                return base_str(self)
            # for simplicity, we only define one operator that
            # propagates expressions
            def __add__(self, other):
                temp = int(self) + int( other)
                if isinstance(self, NamedInt) and isinstance(other, NamedInt):
                    return NamedInt(
                        '({0} + {1})'.format(self.__name__, other.__name__),
                        temp )
                else:
                    return temp

        class NEI(NamedInt, Enum):
            x = ('the-x', 1)
            y = ('the-y', 2)

        self.assertIs(NEI.__new__, Enum.__new__)
        self.assertEqual(repr(NEI.x + NEI.y), "NamedInt('(the-x + the-y)', 3)")
        globals()['NamedInt'] = NamedInt
        globals()['NEI'] = NEI
        NI5 = NamedInt('test', 5)
        self.assertEqual(NI5, 5)
        self.assertEqual(NEI.y.value, 2)
        with self.assertRaises(TypeError):
            dumps(NEI.x)
        with self.assertRaises(PicklingError):
            dumps(NEI)

    def test_tuple_subclass(self):
        class SomeTuple(tuple, Enum):
            first = (1, 'for the money')
            second = (2, 'for the show')
            third = (3, 'for the music')
        self.assertIs(type(SomeTuple.first), SomeTuple)
        self.assertIsInstance(SomeTuple.second, tuple)
        self.assertEqual(SomeTuple.third, (3, 'for the music'))
        globals()['SomeTuple'] = SomeTuple
        self.assertIs(loads(dumps(SomeTuple.first)), SomeTuple.first)

    def test_duplicate_values_give_unique_enum_items(self):
        class AutoNumber(Enum):
            first = ()
            second = ()
            third = ()
            def __new__(cls):
                value = len(cls.__members__) + 1
                obj = object.__new__(cls)
                obj._value = value
                return obj
            def __int__(self):
                return int(self._value)
        self.assertEqual(
                list(AutoNumber),
                [AutoNumber.first, AutoNumber.second, AutoNumber.third],
                )
        self.assertEqual(int(AutoNumber.second), 2)
        self.assertIs(AutoNumber(1), AutoNumber.first)

    def test_inherited_new_from_enhanced_enum(self):
        class AutoNumber(Enum):
            def __new__(cls):
                value = len(cls.__members__) + 1
                obj = object.__new__(cls)
                obj._value = value
                return obj
            def __int__(self):
                return int(self._value)
        class Color(AutoNumber):
            red = ()
            green = ()
            blue = ()
        self.assertEqual(list(Color), [Color.red, Color.green, Color.blue])
        self.assertEqual(list(map(int, Color)), [1, 2, 3])

    def test_inherited_new_from_mixed_enum(self):
        class AutoNumber(IntEnum):
            def __new__(cls):
                value = len(cls.__members__) + 1
                obj = int.__new__(cls, value)
                obj._value = value
                return obj
        class Color(AutoNumber):
            red = ()
            green = ()
            blue = ()
        self.assertEqual(list(Color), [Color.red, Color.green, Color.blue])
        self.assertEqual(list(map(int, Color)), [1, 2, 3])

    def test_ordered_mixin(self):
        class OrderedEnum(Enum):
            def __ge__(self, other):
                if self.__class__ is other.__class__:
                    return self._value >= other._value
                return NotImplemented
            def __gt__(self, other):
                if self.__class__ is other.__class__:
                    return self._value > other._value
                return NotImplemented
            def __le__(self, other):
                if self.__class__ is other.__class__:
                    return self._value <= other._value
                return NotImplemented
            def __lt__(self, other):
                if self.__class__ is other.__class__:
                    return self._value < other._value
                return NotImplemented
        class Grade(OrderedEnum):
            A = 5
            B = 4
            C = 3
            D = 2
            F = 1
        self.assertGreater(Grade.A, Grade.B)
        self.assertLessEqual(Grade.F, Grade.C)
        self.assertLess(Grade.D, Grade.A)
        self.assertGreaterEqual(Grade.B, Grade.B)
    def test_extending2(self):
        class Shade(Enum):
            def shade(self):
                print(self.name)
        class Color(Shade):
            red = 1
            green = 2
            blue = 3
        with self.assertRaises(TypeError):
            class MoreColor(Color):
                cyan = 4
                magenta = 5
                yellow = 6

    def test_extending3(self):
        class Shade(Enum):
            def shade(self):
                return self.name
        class Color(Shade):
            def hex(self):
                return '%s hexlified!' % self.value
        class MoreColor(Color):
            cyan = 4
            magenta = 5
            yellow = 6
        self.assertEqual(MoreColor.magenta.hex(), '5 hexlified!')


    def test_no_duplicates(self):
        class UniqueEnum(Enum):
            def __init__(self, *args):
                cls = self.__class__
                if any(self.value == e.value for e in cls):
                    a = self.name
                    e = cls(self.value).name
                    raise ValueError(
                            "aliases not allowed in UniqueEnum:  %r --> %r"
                            % (a, e)
                            )
        class Color(UniqueEnum):
            red = 1
            green = 2
            blue = 3
        with self.assertRaises(ValueError):
            class Color(UniqueEnum):
                red = 1
                green = 2
                blue = 3
                grene = 2

    def test_init(self):
        class Planet(Enum):
            MERCURY = (3.303e+23, 2.4397e6)
            VENUS   = (4.869e+24, 6.0518e6)
            EARTH   = (5.976e+24, 6.37814e6)
            MARS    = (6.421e+23, 3.3972e6)
            JUPITER = (1.9e+27,   7.1492e7)
            SATURN  = (5.688e+26, 6.0268e7)
            URANUS  = (8.686e+25, 2.5559e7)
            NEPTUNE = (1.024e+26, 2.4746e7)
            def __init__(self, mass, radius):
                self.mass = mass       # in kilograms
                self.radius = radius   # in meters
            @property
            def surface_gravity(self):
                # universal gravitational constant  (m3 kg-1 s-2)
                G = 6.67300E-11
                return G * self.mass / (self.radius * self.radius)
        self.assertEqual(round(Planet.EARTH.surface_gravity, 2), 9.80)
        self.assertEqual(Planet.EARTH.value, (5.976e+24, 6.37814e6))


if __name__ == '__main__':
    unittest.main()
