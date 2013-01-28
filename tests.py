from unittest import TestCase

from spindly import js

class TestSpindly(TestCase):
    def test_javascript_primitives(self):
        self.assertIs(js('null'), None)
        self.assertIs(js('true'), True)
        self.assertIs(js('false'), False)
        self.assertEqual(js('"alpha"'), 'alpha')
        self.assertEqual(js('1'), 1)
        self.assertEqual(js('1.2'), 1.2)
